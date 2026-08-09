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
static Ort::Env ort_env(ORT_LOGGING_LEVEL_WARNING, "BlazeFace");
static Ort::Session* yolo_session = nullptr;
static bool yolo_loaded = false;

// BlazeFace constants
const int INPUT_SIZE = 128;
const float CONF_THRESH = 0.5f;
const float IOU_THRESH = 0.3f;

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

static std::vector<cv::Rect> detectHumans(const cv::Mat &img320)
{
    load_yolo();

    std::vector<cv::Rect> boxes;
    if (!yolo_loaded)
        return boxes;

    // Convert to 128x128 for BlazeFace
    cv::Mat img128;
    cv::resize(img320, img128, cv::Size(INPUT_SIZE, INPUT_SIZE));
    
    cv::Mat rgb;
    cv::cvtColor(img128, rgb, cv::COLOR_BGR2RGB);
    rgb.convertTo(rgb, CV_32F, 1.0f / 255.0f);

    std::vector<float> input_tensor_values(1 * 3 * INPUT_SIZE * INPUT_SIZE);

    for (int y = 0; y < INPUT_SIZE; y++) {
        for (int x = 0; x < INPUT_SIZE; x++) {
            cv::Vec3f p = rgb.at<cv::Vec3f>(y, x);
            input_tensor_values[y * INPUT_SIZE + x] = p[0];  // R
            input_tensor_values[INPUT_SIZE * INPUT_SIZE + y * INPUT_SIZE + x] = p[1];  // G
            input_tensor_values[2 * INPUT_SIZE * INPUT_SIZE + y * INPUT_SIZE + x] = p[2];  // B
        }
    }

    std::array<int64_t, 4> input_shape = {1, 3, INPUT_SIZE, INPUT_SIZE};
    Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);

    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        mem_info,
        input_tensor_values.data(),
        input_tensor_values.size(),
        input_shape.data(),
        input_shape.size()
    );

    Ort::AllocatorWithDefaultOptions allocator;

    auto input_name = yolo_session->GetInputNameAllocated(0, allocator);
    auto output_name = yolo_session->GetOutputNameAllocated(0, allocator);

    const char* input_names[] = {input_name.get()};
    const char* output_names[] = {output_name.get()};

    // Run inference
    auto output_tensors = yolo_session->Run(
        Ort::RunOptions{nullptr},
        input_names, &input_tensor, 1,
        output_names, 1);

    float* out = output_tensors[0].GetTensorMutableData<float>();

    auto shape = output_tensors[0]
        .GetTensorTypeAndShapeInfo()
        .GetShape();

    // Parse BlazeFace output
    struct Detection {
        float x1, y1, x2, y2;
        float confidence;
    };
    std::vector<Detection> detections;

    // Handle different output shapes
    int num_detections = 0;
    bool is_single_detection = false;
    
    if (shape.size() == 3 && shape[0] == 1) {
        // Shape: {1, N, 16}
        num_detections = shape[1];
    } else if (shape.size() == 2 && shape[0] == 1 && shape[1] == 16) {
        // Single detection: {1, 16}
        num_detections = 1;
        is_single_detection = true;
    } else if (shape.size() == 2 && shape[0] > 0 && shape[1] == 16) {
        // Shape: {N, 16}
        num_detections = shape[0];
    } else if (shape.size() == 1 && shape[0] % 16 == 0) {
        // Flat array
        num_detections = shape[0] / 16;
    } else {
        // Try to get from scores output
        auto scores_shape = output_tensors[1].GetTensorTypeAndShapeInfo().GetShape();
        if (!scores_shape.empty()) {
            num_detections = scores_shape[0];
        }
    }

    // Parse detections
    for (int i = 0; i < num_detections; i++) {
        float y1, x1, y2, x2;
        float score = 0.0f;
        
        if (is_single_detection) {
            // Single detection
            y1 = out[0];
            x1 = out[1];
            y2 = out[2];
            x2 = out[3];
            // Get score if available
            if (output_tensors.size() > 1) {
                float* scores = output_tensors[1].GetTensorMutableData<float>();
                score = scores[0];
            } else {
                score = 1.0f;
            }
        } else if (shape.size() == 3) {
            // Shape: {1, N, 16}
            int offset = i * 16;
            y1 = out[offset + 0];
            x1 = out[offset + 1];
            y2 = out[offset + 2];
            x2 = out[offset + 3];
            
            // Get score if available
            if (output_tensors.size() > 1) {
                float* scores = output_tensors[1].GetTensorMutableData<float>();
                score = scores[i];
            } else {
                score = 1.0f;
            }
        } else {
            // Flat array or other format
            int offset = i * 16;
            y1 = out[offset + 0];
            x1 = out[offset + 1];
            y2 = out[offset + 2];
            x2 = out[offset + 3];
            score = 1.0f;
        }
        
        // Apply confidence threshold
        if (score < CONF_THRESH) continue;
        
        // Validate detection
        if (x1 < 0 || x1 > 1 || x2 < 0 || x2 > 1 ||
            y1 < 0 || y1 > 1 || y2 < 0 || y2 > 1 ||
            x2 <= x1 || y2 <= y1) {
            continue;
        }
        
        // Convert from normalized to pixel coordinates (320x320)
        float px1 = x1 * img320.cols;
        float py1 = y1 * img320.rows;
        float px2 = x2 * img320.cols;
        float py2 = y2 * img320.rows;
        
        detections.push_back({px1, py1, px2, py2, score});
    }

    // Apply NMS
    if (!detections.empty()) {
        std::vector<cv::Rect> rects;
        std::vector<float> scores;
        
        for (const auto& det : detections) {
            rects.emplace_back(
                (int)det.x1, 
                (int)det.y1,
                (int)(det.x2 - det.x1),
                (int)(det.y2 - det.y1)
            );
            scores.push_back(det.confidence);
        }
        
        std::vector<int> indices;
        cv::dnn::NMSBoxes(rects, scores, CONF_THRESH, IOU_THRESH, indices);
        
        for (int idx : indices) {
            boxes.push_back(rects[idx]);
        }
    }

    return boxes;
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

    cv::putText(frame_detection, "Faces: " + std::to_string(boxes.size()),
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