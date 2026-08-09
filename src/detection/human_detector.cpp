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
            input_tensor_values[y * 320 + x] = p[0];  // R
            input_tensor_values[320 * 320 + y * 320 + x] = p[1];  // G
            input_tensor_values[2 * 320 * 320 + y * 320 + x] = p[2];  // B
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

    // Run inference
    auto output_tensors = yolo_session->Run(
        Ort::RunOptions{nullptr},
        input_names, &input_tensor, 1,
        output_names, 1);

    float* out = output_tensors[0].GetTensorMutableData<float>();

    auto shape = output_tensors[0]
        .GetTensorTypeAndShapeInfo()
        .GetShape();

    int channels = shape[1];        // 84 (4 bbox + 80 class)
    int num_predictions = shape[2]; // 2100

    struct Detection {
        float x1, y1, x2, y2;
        float confidence;
        int class_id;
    };
    std::vector<Detection> detections;

    // Process each prediction
    for (int i = 0; i < num_predictions; i++) {
        // Get bounding box coordinates (already in pixel space)
        float x = out[0 * num_predictions + i];
        float y = out[1 * num_predictions + i];
        float w = out[2 * num_predictions + i];
        float h = out[3 * num_predictions + i];

        // Find best class from raw confidence scores
        float best_score = 0.0f;
        int best_class = -1;
        for (int c = 4; c < channels; c++) {
            float score = out[c * num_predictions + i];
            if (score > best_score) {
                best_score = score;
                best_class = c - 4;
            }
        }

        // Filter: only person class with confidence threshold
        if (best_class != 0) continue;
        if (best_score < 0.25f) continue;

        // Convert from center format to corner format and scale
float x1 = x - w/2;
float y1 = y - h/2;
float x2 = x + w/2;
float y2 = y + h/2;

// undo padding
x1 -= pad_x;
x2 -= pad_x;
y1 -= pad_y;
y2 -= pad_y;

// undo scaling
x1 /= scale;
x2 /= scale;
y1 /= scale;
y2 /= scale;

        
        detections.push_back({x1, y1, x2, y2, best_score, best_class});
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
        cv::dnn::NMSBoxes(rects, scores, 0.25f, 0.45f, indices);
        
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
int new_w = 320, new_h = 320;
int w = frame_source.cols;
int h = frame_source.rows;

float scale = std::min((float)new_w / w, (float)new_h / h);
int resized_w = int(w * scale);
int resized_h = int(h * scale);

cv::Mat resized;
cv::resize(frame_source, resized, cv::Size(resized_w, resized_h));

cv::Mat frame_detection = cv::Mat::zeros(new_h, new_w, frame_source.type());
int pad_x = (new_w - resized_w) / 2;
int pad_y = (new_h - resized_h) / 2;

resized.copyTo(frame_detection(cv::Rect(pad_x, pad_y, resized_w, resized_h)));


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