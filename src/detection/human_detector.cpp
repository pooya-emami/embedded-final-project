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
    rgb.convertTo(rgb, CV_32F, 1.0f / 255.0f);

    std::vector<float> input_tensor_values(1 * 3 * 320 * 320);

    for (int y = 0; y < 320; y++) {
        for (int x = 0; x < 320; x++) {
            cv::Vec3f p = rgb.at<cv::Vec3f>(y, x);
            input_tensor_values[y * 320 + x] = p[0];
            input_tensor_values[320 * 320 + y * 320 + x] = p[1];
            input_tensor_values[2 * 320 * 320 + y * 320 + x] = p[2];
        }
    }

    std::array<int64_t, 4> input_shape = {1, 3, 320, 320};

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

    auto output_tensors = yolo_session->Run(
        Ort::RunOptions{nullptr},
        input_names, &input_tensor, 1,
        output_names, 1);

    float* out = output_tensors[0].GetTensorMutableData<float>();

    auto shape = output_tensors[0]
        .GetTensorTypeAndShapeInfo()
        .GetShape();

    // Debug print
    printf("Output shape: [");
    for (size_t i = 0; i < shape.size(); i++) {
        printf("%ld ", shape[i]);
    }
    printf("]\n");

    // For shape [1, 84, 2100]
    int channels = shape[1];  // 84
    int num_predictions = shape[2];  // 2100

    // Store all detections
    struct Detection {
        float x1, y1, x2, y2;
        float confidence;
    };
    std::vector<Detection> detections;

    // Process each prediction (like Python's transpose)
    for (int i = 0; i < num_predictions; i++) {
        // Get the 84 values for this prediction (transposed)
        float x = out[0 * num_predictions + i];
        float y = out[1 * num_predictions + i];
        float w = out[2 * num_predictions + i];
        float h = out[3 * num_predictions + i];
        float obj = out[4 * num_predictions + i];
        
        if (obj < 0.25f) continue;
        
        // Find best class
        float best_score = 0;
        int best_class = -1;
        for (int c = 5; c < channels; c++) {
            float s = out[c * num_predictions + i];
            if (s > best_score) {
                best_score = s;
                best_class = c - 5;
            }
        }
        
        // Only person class (class 0)
        if (best_class != 0) continue;
        if (best_score < 0.25f) continue;
        
        // Convert center format to corner format and scale to image size
        float x1 = (x - w/2) * img320.cols / 320.0f;
        float y1 = (y - h/2) * img320.rows / 320.0f;
        float x2 = (x + w/2) * img320.cols / 320.0f;
        float y2 = (y + h/2) * img320.rows / 320.0f;
        
        detections.push_back({x1, y1, x2, y2, best_score});
    }

    printf("Raw detections: %zu\n", detections.size());

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
        cv::dnn::NMSBoxes(rects, scores, 0.25f, 0.45f, indices);
        
        for (int idx : indices) {
            boxes.push_back(rects[idx]);
        }
    }

    printf("After NMS: %zu persons\n", boxes.size());
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