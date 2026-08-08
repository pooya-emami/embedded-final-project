#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <onnxruntime_cxx_api.h>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <sstream>
#include <iomanip>
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


cv::Mat rgb;
cv::cvtColor(img320, rgb, cv::COLOR_BGR2RGB);

rgb.convertTo(rgb, CV_32F, 1.0/255.0);

std::vector<float> input_tensor_values(1 * 3 * 320 * 320);

for(int y = 0; y < 320; y++)
{
    for(int x = 0; x < 320; x++)
    {
        cv::Vec3f pixel = rgb.at<cv::Vec3f>(y,x);

        input_tensor_values[
            0*320*320 + y*320 + x
        ] = pixel[0];

        input_tensor_values[
            1*320*320 + y*320 + x
        ] = pixel[1];

        input_tensor_values[
            2*320*320 + y*320 + x
        ] = pixel[2];
    }
}

    std::array<int64_t,4> input_shape = {
        1,3,320,320
    };


    size_t input_tensor_size = 1*3*320*320;


    Ort::MemoryInfo memory_info =
        Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator,
            OrtMemTypeDefault
        );


    Ort::Value input_tensor =
        Ort::Value::CreateTensor<float>(
            memory_info,
            (float*)blob.data,
            input_tensor_size,
            input_shape.data(),
            input_shape.size()
        );


    Ort::AllocatorWithDefaultOptions allocator;


    auto input_name =
        yolo_session->GetInputNameAllocated(
            0,
            allocator
        );

    auto output_name =
        yolo_session->GetOutputNameAllocated(
            0,
            allocator
        );


    const char* input_names[] = {
        input_name.get()
    };

    const char* output_names[] = {
        output_name.get()
    };


    auto output_tensors =
        yolo_session->Run(
            Ort::RunOptions{nullptr},
            input_names,
            &input_tensor,
            1,
            output_names,
            1
        );


    float* output =
        output_tensors[0].GetTensorMutableData<float>();


    auto output_shape =
        output_tensors[0]
        .GetTensorTypeAndShapeInfo()
        .GetShape();


    std::cout << "YOLO output: ";

    for(auto x: output_shape)
        std::cout << x << " ";

    std::cout << std::endl;


    return boxes;
}

extern "C" DetectionResult process_frame(
    const uint8_t* jpeg_data,
    size_t jpeg_len,
    int output_width,
    int output_height)
{
    DetectionResult result = {};

    std::vector<uint8_t> buf(jpeg_data, jpeg_data + jpeg_len);
    cv::Mat frame_source = cv::imdecode(buf, cv::IMREAD_COLOR);

    if (frame_source.empty())
        return result;

    cv::Mat frame_detection;
    cv::resize(frame_source, frame_detection, cv::Size(320, 320));

    auto boxes = detectHumans(frame_detection);

    for (const auto &box : boxes)
        cv::rectangle(frame_detection, box, cv::Scalar(0, 255, 0), 2);

    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(
        std::localtime(&now_time_t),
        "%Y-%m-%d %H:%M:%S"
    );

    cv::putText(
        frame_detection,
        "Student: 404300409",
        cv::Point(10, 30),
        cv::FONT_HERSHEY_SIMPLEX,
        0.6,
        cv::Scalar(0, 255, 255),
        2
    );

    cv::putText(
        frame_detection,
        ss.str(),
        cv::Point(10, 60),
        cv::FONT_HERSHEY_SIMPLEX,
        0.6,
        cv::Scalar(255, 255, 255),
        2
    );

    cv::putText(
        frame_detection,
        "Persons: " + std::to_string(boxes.size()),
        cv::Point(10, 90),
        cv::FONT_HERSHEY_SIMPLEX,
        0.6,
        cv::Scalar(0, 255, 0),
        2
    );

    static double fps = 0;
    static int frame_count = 0;
    static auto start = std::chrono::steady_clock::now();

    auto current = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(
        current - start
    ).count();

    frame_count++;

    if (elapsed >= 1.0) {
        fps = frame_count / elapsed;
        frame_count = 0;
        start = current;
    }

    cv::putText(
        frame_detection,
        "FPS: " + std::to_string(fps).substr(0, 4),
        cv::Point(frame_detection.cols - 100, 30),
        cv::FONT_HERSHEY_SIMPLEX,
        0.6,
        cv::Scalar(0, 255, 0),
        2
    );

    cv::Mat frame_output;

    cv::resize(
        frame_detection,
        frame_output,
        cv::Size(output_width, output_height)
    );

    std::vector<uint8_t> jpegBuf;

    cv::imencode(
        ".jpg",
        frame_output,
        jpegBuf
    );

    result.jpeg_output = (uint8_t*)malloc(jpegBuf.size());

    if (result.jpeg_output) {
        result.jpeg_length = jpegBuf.size();
        memcpy(
            result.jpeg_output,
            jpegBuf.data(),
            jpegBuf.size()
        );
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