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

static std::vector<cv::Rect> detectHumans(const cv::Mat &img320)
{
    load_yolo(); // reuse your loader but point YOLO_MODEL_FILE to blaze.onnx

    std::vector<cv::Rect> boxes;
    if (!yolo_loaded)
        return boxes;

    // Preprocess
    cv::Mat rgb;
    cv::cvtColor(img320, rgb, cv::COLOR_BGR2RGB);
    rgb.convertTo(rgb, CV_32F, 1.0f / 255.0f);

    std::vector<float> input_tensor_values(1 * 3 * 128 * 128);

    cv::Mat resized;
    cv::resize(rgb, resized, cv::Size(128, 128));

    int idx = 0;
    for (int c = 0; c < 3; c++)
        for (int y = 0; y < 128; y++)
            for (int x = 0; x < 128; x++)
                input_tensor_values[idx++] = resized.at<cv::Vec3f>(y, x)[c];

    std::array<int64_t, 4> input_shape = {1, 3, 128, 128};
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

    // BlazeFace has 3 outputs
    auto out_scores_name = yolo_session->GetOutputNameAllocated(0, allocator);
    auto out_boxes_name  = yolo_session->GetOutputNameAllocated(1, allocator);
    auto out_nms_name    = yolo_session->GetOutputNameAllocated(2, allocator);

    const char* input_names[] = {input_name.get()};
    const char* output_names[] = {
        out_scores_name.get(),
        out_boxes_name.get(),
        out_nms_name.get()
    };

    auto output_tensors = yolo_session->Run(
        Ort::RunOptions{nullptr},
        input_names, &input_tensor, 1,
        output_names, 3
    );

    // selectedBoxes = NMS output
    float* selected = output_tensors[2].GetTensorMutableData<float>();

    auto shape = output_tensors[2]
        .GetTensorTypeAndShapeInfo()
        .GetShape();

    int num_selected = shape[1];   // number of final detections
    int fields = shape[2];         // 16 fields per detection

    for (int i = 0; i < num_selected; i++) {
        float* det = selected + i * fields;

        float y1 = det[0];
        float x1 = det[1];
        float y2 = det[2];
        float x2 = det[3];

        // BlazeFace outputs normalized coordinates (0..1)
        int X1 = x1 * img320.cols;
        int Y1 = y1 * img320.rows;
        int X2 = x2 * img320.cols;
        int Y2 = y2 * img320.rows;

        boxes.emplace_back(
            X1,
            Y1,
            X2 - X1,
            Y2 - Y1
        );
    }

    return boxes;
}


extern "C" void free_detection_result(DetectionResult* res)
{
    if (res && res->jpeg_output) {
        free(res->jpeg_output);
        res->jpeg_output = NULL;
        res->jpeg_length = 0;
    }
}