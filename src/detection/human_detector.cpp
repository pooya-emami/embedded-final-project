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

    // Print model input/output info
    printf("\n=== Model Input/Output Info ===\n");
    size_t num_inputs = yolo_session->GetInputCount();
    size_t num_outputs = yolo_session->GetOutputCount();
    printf("Inputs: %zu, Outputs: %zu\n", num_inputs, num_outputs);
    
    for (size_t i = 0; i < num_inputs; i++) {
        auto name = yolo_session->GetInputNameAllocated(i, allocator);
        auto info = yolo_session->GetInputTypeInfo(i);
        auto shape_info = info.GetTensorTypeAndShapeInfo();
        auto shape = shape_info.GetShape();
        printf("Input %zu: %s, shape: [", i, name.get());
        for (auto dim : shape) printf("%ld ", dim);
        printf("]\n");
    }
    
    for (size_t i = 0; i < num_outputs; i++) {
        auto name = yolo_session->GetOutputNameAllocated(i, allocator);
        auto info = yolo_session->GetOutputTypeInfo(i);
        auto shape_info = info.GetTensorTypeAndShapeInfo();
        auto shape = shape_info.GetShape();
        printf("Output %zu: %s, shape: [", i, name.get());
        for (auto dim : shape) printf("%ld ", dim);
        printf("]\n");
    }
    printf("===============================\n");

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

    printf("Output shape: [");
    for (size_t i = 0; i < shape.size(); i++) {
        printf("%ld ", shape[i]);
    }
    printf("]\n");

    int channels = shape[1];  // 84
    int num_predictions = shape[2];  // 2100

    // DEBUG: Print ALL values for first prediction
    printf("\n=== First prediction ALL 84 values ===\n");
    for (int c = 0; c < channels; c++) {
        float val = out[c * num_predictions + 0];
        printf("idx %d: %.10f ", c, val);
        if ((c+1) % 8 == 0) printf("\n");
    }
    printf("\n");

    // DEBUG: Print first 10 predictions, first 10 values
    printf("\n=== First 10 predictions (first 10 values) ===\n");
    for (int i = 0; i < 10 && i < num_predictions; i++) {
        printf("Pred %d: ", i);
        for (int c = 0; c < 10 && c < channels; c++) {
            printf("%.10f ", out[c * num_predictions + i]);
        }
        printf("\n");
    }

    // DEBUG: Check if any value is > 0.01
    int values_above_001 = 0;
    printf("\n=== Values > 0.01 ===\n");
    for (int i = 0; i < std::min(100, num_predictions); i++) {
        for (int c = 0; c < channels; c++) {
            float val = out[c * num_predictions + i];
            if (val > 0.01f) {
                values_above_001++;
                if (values_above_001 <= 20) {
                    printf("out[%d*%d+%d] = %.10f\n", c, num_predictions, i, val);
                }
            }
        }
    }
    printf("Total values > 0.01 in first 100 predictions: %d\n", values_above_001);

    // DEBUG: Check values around the middle of the array
    printf("\n=== Values at position 1000 ===\n");
    for (int c = 0; c < 10; c++) {
        printf("out[%d*%d+%d] = %.10f\n", c, num_predictions, 1000, out[c * num_predictions + 1000]);
    }

    // Try to detect if output needs sigmoid or softmax
    // Check if values are raw logits (could be negative)
    float max_val = -1000, min_val = 1000;
    for (int i = 0; i < std::min(100, num_predictions); i++) {
        for (int c = 0; c < channels; c++) {
            float val = out[c * num_predictions + i];
            if (val > max_val) max_val = val;
            if (val < min_val) min_val = val;
        }
    }
    printf("\n=== Value Range ===\n");
    printf("Min value: %.10f, Max value: %.10f\n", min_val, max_val);
    printf("If values are negative or large (>10), they need sigmoid/softmax\n");

    // Store all detections
    struct Detection {
        float x1, y1, x2, y2;
        float confidence;
        int class_id;
    };
    std::vector<Detection> detections;

    // Try both with and without sigmoid
    bool use_sigmoid = false;
    
    // If max value is > 10 or min value is negative, we probably need sigmoid
    if (max_val > 10.0f || min_val < -10.0f) {
        use_sigmoid = true;
        printf("\n=== Using sigmoid activation (values look like logits) ===\n");
    } else if (max_val < 0.1f && min_val >= 0) {
        printf("\n=== Values are already probabilities (0-1 range) ===\n");
    }

    auto sigmoid = [](float x) { return 1.0f / (1.0f + exp(-x)); };

    // Process each prediction
    for (int i = 0; i < num_predictions; i++) {
        float x = out[0 * num_predictions + i];
        float y = out[1 * num_predictions + i];
        float w = out[2 * num_predictions + i];
        float h = out[3 * num_predictions + i];
        
        // Find best class score
        float best_score = 0;
        int best_class = -1;
        for (int c = 4; c < channels; c++) {
            float s = out[c * num_predictions + i];
            if (use_sigmoid) {
                s = sigmoid(s);
            }
            if (s > best_score) {
                best_score = s;
                best_class = c - 4;
            }
        }
        
        // Apply threshold
        if (best_score < 0.25f) continue;
        
        // Only person class (class 0)
        if (best_class != 0) continue;
        
        // Convert center format to corner format and scale
        float x1 = (x - w/2) * img320.cols / 320.0f;
        float y1 = (y - h/2) * img320.rows / 320.0f;
        float x2 = (x + w/2) * img320.cols / 320.0f;
        float y2 = (y + h/2) * img320.rows / 320.0f;
        
        detections.push_back({x1, y1, x2, y2, best_score, best_class});
        
        // Debug: Print first few detections
        if (detections.size() <= 10) {
            printf("Detection %zu: class=%d, score=%.4f, box=(%.1f,%.1f)-(%.1f,%.1f)\n", 
                   detections.size(), best_class, best_score, x1, y1, x2, y2);
        }
    }

    printf("\n=== Detection Summary ===\n");
    printf("Detections before NMS: %zu\n", detections.size());

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
        
        printf("Indices after NMS: %zu\n", indices.size());
        
        for (int idx : indices) {
            boxes.push_back(rects[idx]);
        }
    }

    printf("Final boxes: %zu\n", boxes.size());
    printf("========================\n\n");
    
    return boxes;
}

// This is the function that server.c is calling
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

    if (frame_source.empty())
        return result;

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