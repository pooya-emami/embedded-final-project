#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/dnn.hpp>

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include "human_detector.hpp"

static char model_path[256] = {0};
static Ort::Env ort_env(ORT_LOGGING_LEVEL_WARNING, "YOLO");
static Ort::Session* yolo_session = nullptr;
static bool yolo_loaded = false;

static void load_yolo()
{
    if (yolo_loaded)
        return;

    snprintf(model_path, sizeof(model_path),
             "%s%s", MODEL_BASE_PATH, YOLO_MODEL_FILE);

    Ort::SessionOptions session_options;
    session_options.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_ALL
    );
    session_options.SetIntraOpNumThreads(4);
    session_options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);

    yolo_session = new Ort::Session(
        ort_env,
        model_path,
        session_options
    );

    yolo_loaded = true;
}

DetectionResult detectHumans(const cv::Mat& frame)
{
    DetectionResult result;
    result.person_count = 0;

    // Resize to 128x128
    cv::Mat resized;
    cv::resize(frame, resized, cv::Size(128, 128));

    // Convert to float32
    cv::Mat input;
    resized.convertTo(input, CV_32F, 1.0 / 255.0);

    // CHW
    std::vector<float> chw(128 * 128 * 3);
    for (int c = 0; c < 3; c++)
        for (int y = 0; y < 128; y++)
            for (int x = 0; x < 128; x++)
                chw[c * 128 * 128 + y * 128 + x] = input.at<cv::Vec3f>(y, x)[c];

    Ort::AllocatorWithDefaultOptions allocator;

    // Prepare inputs
    std::array<int64_t, 4> img_shape = {1, 3, 128, 128};
    Ort::Value img_tensor = Ort::Value::CreateTensor<float>(
        allocator, chw.data(), chw.size(), img_shape.data(), img_shape.size());

    float conf_thr = 0.5f;
    int64_t max_det = 50;
    float iou_thr = 0.3f;

    Ort::Value conf_tensor = Ort::Value::CreateTensor<float>(
        allocator, &conf_thr, 1, std::array<int64_t,1>{1}.data(), 1);

    Ort::Value maxdet_tensor = Ort::Value::CreateTensor<int64_t>(
        allocator, &max_det, 1, std::array<int64_t,1>{1}.data(), 1);

    Ort::Value iou_tensor = Ort::Value::CreateTensor<float>(
        allocator, &iou_thr, 1, std::array<int64_t,1>{1}.data(), 1);

    const char* input_names[] = {
        "image",
        "conf_threshold",
        "max_detections",
        "iou_threshold"
    };

    const char* output_names[] = {"selectedBoxes"};

    auto output = yolo_session->Run(
        Ort::RunOptions{nullptr},
        input_names,
        (const Ort::Value*[]) { &img_tensor, &conf_tensor, &maxdet_tensor, &iou_tensor },
        4,
        output_names,
        1
    );

    // Parse selectedBoxes
    auto& boxes_tensor = output[0];
    auto info = boxes_tensor.GetTensorTypeAndShapeInfo();
    auto shape = info.GetShape();

    int num_boxes = shape[1]; // 896
    int fields = shape[2];    // 16

    const float* data = boxes_tensor.GetTensorData<float>();

    for (int i = 0; i < num_boxes; i++) {
        float score = data[i * fields + 4];
        if (score < 0.5f) continue;

        float xmin = data[i * fields + 0] * frame.cols;
        float ymin = data[i * fields + 1] * frame.rows;
        float xmax = data[i * fields + 2] * frame.cols;
        float ymax = data[i * fields + 3] * frame.rows;

        result.boxes.push_back({xmin, ymin, xmax, ymax});
    }

    result.person_count = result.boxes.size();
    return result;
}



extern "C" DetectionResult process_frame(
    const uint8_t* jpeg_data,
    size_t jpeg_len,
    int output_width,
    int output_height)
{
    DetectionResult result = {};
    result.jpeg_output = NULL;
    result.jpeg_length = 0;
    result.width = 0;
    result.height = 0;
    result.person_count = 0;

    std::vector<uint8_t> buf(jpeg_data, jpeg_data + jpeg_len);
    cv::Mat frame_source = cv::imdecode(buf, cv::IMREAD_COLOR);

    if (frame_source.empty()) {
        return result;
    }

    cv::Mat frame_detection;
    cv::resize(frame_source, frame_detection, cv::Size(320, 320));

    auto boxes = detectHumans(frame_detection);

    // Draw boxes
    for (const auto &box : boxes)
        cv::rectangle(frame_detection, box, cv::Scalar(0, 255, 0), 2);

    // Add text overlays
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_time_t), "%Y-%m-%d %H:%M:%S");

    cv::putText(frame_detection, "Student: 404300409",
                cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX,
                0.6, cv::Scalar(0, 255, 255), 2);

    cv::putText(frame_detection, ss.str(),
                cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX,
                0.6, cv::Scalar(255, 255, 255), 2);

    cv::putText(frame_detection, "Persons: " + std::to_string(boxes.size()),
                cv::Point(10, 90), cv::FONT_HERSHEY_SIMPLEX,
                0.6, cv::Scalar(0, 255, 0), 2);

    // FPS counter
    static double fps = 0;
    static int frame_count = 0;
    static auto start = std::chrono::steady_clock::now();

    auto current = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(current - start).count();
    frame_count++;

    if (elapsed >= 1.0) {
        fps = frame_count / elapsed;
        frame_count = 0;
        start = current;
    }

    std::string fps_str = std::to_string(fps).substr(0, 4);
    cv::putText(frame_detection, "FPS: " + fps_str,
                cv::Point(frame_detection.cols - 100, 30),
                cv::FONT_HERSHEY_SIMPLEX,
                0.6, cv::Scalar(0, 255, 0), 2);

    // Resize to output size and encode as JPEG
    cv::Mat frame_output;
    cv::resize(frame_detection, frame_output, cv::Size(output_width, output_height));

    std::vector<uint8_t> jpegBuf;
    cv::imencode(".jpg", frame_output, jpegBuf);

    result.jpeg_output = (uint8_t*)malloc(jpegBuf.size());
    if (result.jpeg_output) {
        result.jpeg_length = jpegBuf.size();
        memcpy(result.jpeg_output, jpegBuf.data(), jpegBuf.size());
    }

    result.width = output_width;
    result.height = output_height;
    result.person_count = (int)boxes.size();

    return result;
}

extern "C" void free_detection_result(DetectionResult* res)
{
    if (res && res->jpeg_output) {
        free(res->jpeg_output);
        res->jpeg_output = NULL;
        res->jpeg_length = 0;
    }
}