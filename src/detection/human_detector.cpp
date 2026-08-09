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
#include <cmath>

#include "human_detector.hpp"

static char model_path[256] = {0};
static Ort::Env ort_env(ORT_LOGGING_LEVEL_WARNING, "BlazeFace");
static Ort::Session* blaze_session = nullptr;
static bool blaze_loaded = false;

// Store input/output names as static strings
static std::vector<std::string> input_names_str;
static std::vector<std::string> output_names_str;

// BlazeFace configuration
static const float CONF_THRESH = 0.1f;
static const float IOU_THRESH = 0.3f;
static const int MAX_DET = 25;
static const int INPUT_SIZE = 128;  // BlazeFace expects 128x128

static void load_blaze()
{
    if (blaze_loaded)
        return;

    snprintf(model_path, sizeof(model_path),
             "%s%s", MODEL_BASE_PATH, "blaze.onnx");

    Ort::SessionOptions session_options;
    session_options.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_ALL
    );
    session_options.SetIntraOpNumThreads(4);
    session_options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);

    blaze_session = new Ort::Session(
        ort_env,
        model_path,
        session_options
    );

    blaze_loaded = true;
    
    // Store input names
    Ort::AllocatorWithDefaultOptions allocator;
    for (size_t i = 0; i < blaze_session->GetInputCount(); i++) {
        auto name_allocated = blaze_session->GetInputNameAllocated(i, allocator);
        input_names_str.push_back(std::string(name_allocated.get()));
        printf("Input %zu name: '%s'\n", i, input_names_str.back().c_str());
    }
    
    // Store output names
    for (size_t i = 0; i < blaze_session->GetOutputCount(); i++) {
        auto name_allocated = blaze_session->GetOutputNameAllocated(i, allocator);
        output_names_str.push_back(std::string(name_allocated.get()));
        printf("Output %zu name: '%s'\n", i, output_names_str.back().c_str());
    }
    
    // Print model info for debugging
    printf("BlazeFace model loaded successfully!\n");
    printf("Inputs:\n");
    for (size_t i = 0; i < blaze_session->GetInputCount(); i++) {
        auto info = blaze_session->GetInputTypeInfo(i);
        auto shape_info = info.GetTensorTypeAndShapeInfo();
        auto shape = shape_info.GetShape();
        printf("  Input %zu: shape=[", i);
        for (size_t j = 0; j < shape.size(); j++) {
            printf("%lld%s", (long long)shape[j], j < shape.size()-1 ? "," : "");
        }
        printf("]\n");
    }
    printf("Outputs:\n");
    for (size_t i = 0; i < blaze_session->GetOutputCount(); i++) {
        auto info = blaze_session->GetOutputTypeInfo(i);
        auto shape_info = info.GetTensorTypeAndShapeInfo();
        auto shape = shape_info.GetShape();
        printf("  Output %zu: shape=[", i);
        for (size_t j = 0; j < shape.size(); j++) {
            printf("%lld%s", (long long)shape[j], j < shape.size()-1 ? "," : "");
        }
        printf("]\n");
    }
}

// Helper function: Letterbox to square (maintains aspect ratio)
static cv::Mat letterbox_to_square(const cv::Mat& img, int size = INPUT_SIZE) {
    int h = img.rows;
    int w = img.cols;
    float scale = static_cast<float>(size) / std::max(h, w);
    int new_w = static_cast<int>(std::round(w * scale));
    int new_h = static_cast<int>(std::round(h * scale));
    
    cv::Mat resized;
    cv::resize(img, resized, cv::Size(new_w, new_h));
    
    cv::Mat canvas = cv::Mat::ones(size, size, CV_8UC3) * 114;  // Pad color 114,114,114
    int top = (size - new_h) / 2;
    int left = (size - new_w) / 2;
    resized.copyTo(canvas(cv::Rect(left, top, new_w, new_h)));
    
    return canvas;
}

static std::vector<cv::Rect> detectFaces(const cv::Mat &img320)
{
    load_blaze();

    std::vector<cv::Rect> boxes;
    if (!blaze_loaded) {
        printf("BlazeFace not loaded!\n");
        return boxes;
    }

    // Resize to 128x128 with letterboxing
    cv::Mat img128 = letterbox_to_square(img320, INPUT_SIZE);
    
    // Convert BGR to RGB (BlazeFace expects RGB)
    cv::Mat rgb;
    cv::cvtColor(img128, rgb, cv::COLOR_BGR2RGB);
    rgb.convertTo(rgb, CV_32F, 1.0f / 255.0f);

    // Prepare input tensor (1, 3, 128, 128)
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

    // Prepare input tensors
    std::vector<Ort::Value> input_tensors;
    
    // Create image tensor
    Ort::Value image_tensor = Ort::Value::CreateTensor<float>(
        mem_info,
        input_tensor_values.data(),
        input_tensor_values.size(),
        input_shape.data(),
        input_shape.size()
    );
    input_tensors.push_back(std::move(image_tensor));
    
    // Create conf_threshold tensor (float)
    std::vector<float> conf_data = {CONF_THRESH};
    Ort::Value conf_tensor = Ort::Value::CreateTensor<float>(
        mem_info,
        conf_data.data(),
        conf_data.size(),
        std::array<int64_t, 1>{1}.data(),
        1
    );
    input_tensors.push_back(std::move(conf_tensor));
    
    // Create max_detections tensor (int64)
    std::vector<int64_t> max_data = {MAX_DET};
    Ort::Value max_tensor = Ort::Value::CreateTensor<int64_t>(
        mem_info,
        max_data.data(),
        max_data.size(),
        std::array<int64_t, 1>{1}.data(),
        1
    );
    input_tensors.push_back(std::move(max_tensor));
    
    // Create iou_threshold tensor (float)
    std::vector<float> iou_data = {IOU_THRESH};
    Ort::Value iou_tensor = Ort::Value::CreateTensor<float>(
        mem_info,
        iou_data.data(),
        iou_data.size(),
        std::array<int64_t, 1>{1}.data(),
        1
    );
    input_tensors.push_back(std::move(iou_tensor));

    // Prepare input/output name arrays (using static strings)
    std::vector<const char*> input_names;
    for (const auto& name : input_names_str) {
        input_names.push_back(name.c_str());
    }
    
    std::vector<const char*> output_names;
    for (const auto& name : output_names_str) {
        output_names.push_back(name.c_str());
    }

    // Run inference
    try {
        auto output_tensors = blaze_session->Run(
            Ort::RunOptions{nullptr},
            input_names.data(), input_tensors.data(), input_tensors.size(),
            output_names.data(), output_names.size());

        // Process outputs
        if (output_tensors.size() < 1) {
            printf("No output tensors!\n");
            return boxes;
        }

        // Get boxes
        float* boxes_data = output_tensors[0].GetTensorMutableData<float>();
        auto boxes_shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();
        
        printf("Boxes shape: [");
        for (size_t i = 0; i < boxes_shape.size(); i++) {
            printf("%lld%s", (long long)boxes_shape[i], i < boxes_shape.size()-1 ? "," : "");
        }
        printf("]\n");

        // Get scores if available
        float* scores_data = nullptr;
        size_t num_scores = 0;
        if (output_tensors.size() > 1) {
            scores_data = output_tensors[1].GetTensorMutableData<float>();
            auto scores_shape = output_tensors[1].GetTensorTypeAndShapeInfo().GetShape();
            printf("Scores shape: [");
            for (size_t i = 0; i < scores_shape.size(); i++) {
                printf("%lld%s", (long long)scores_shape[i], i < scores_shape.size()-1 ? "," : "");
            }
            printf("]\n");
            if (scores_shape.size() > 0) {
                num_scores = scores_shape[0];
            }
        }

        // Parse detections - shape is [1, 896, 16]
        if (boxes_shape.size() == 3 && boxes_shape[0] == 1) {
            int num_detections = boxes_shape[1];
            int coord_size = boxes_shape[2];
            
            printf("Processing %d detections with %d coordinates each\n", num_detections, coord_size);
            
            struct Detection {
                cv::Rect rect;
                float score;
            };
            std::vector<Detection> detections;
            
            for (int i = 0; i < num_detections; i++) {
                int idx = i * coord_size;
                float y1 = boxes_data[idx + 0];
                float x1 = boxes_data[idx + 1];
                float y2 = boxes_data[idx + 2];
                float x2 = boxes_data[idx + 3];
                
                // Skip invalid boxes
                if ((y2 - y1) < 0.01f || (x2 - x1) < 0.01f) {
                    continue;
                }
                
                // Get score
                float score = 1.0f;
                if (scores_data && i < num_scores) {
                    score = scores_data[i];
                }
                
                if (score < CONF_THRESH) {
                    continue;
                }
                
                // Scale from normalized [0,1] to image coordinates
                int px1 = static_cast<int>(x1 * img320.cols);
                int py1 = static_cast<int>(y1 * img320.rows);
                int px2 = static_cast<int>(x2 * img320.cols);
                int py2 = static_cast<int>(y2 * img320.rows);
                
                if (px1 < px2 && py1 < py2) {
                    detections.push_back({cv::Rect(px1, py1, px2 - px1, py2 - py1), score});
                }
            }
            
            printf("Found %zu valid detections before NMS\n", detections.size());
            
            // Apply NMS
            if (!detections.empty()) {
                std::vector<cv::Rect> rects;
                std::vector<float> scores;
                
                for (const auto& det : detections) {
                    rects.push_back(det.rect);
                    scores.push_back(det.score);
                }
                
                std::vector<int> indices;
                cv::dnn::NMSBoxes(rects, scores, CONF_THRESH, IOU_THRESH, indices);
                
                for (int idx : indices) {
                    boxes.push_back(rects[idx]);
                }
            }
        } else {
            printf("Unexpected output shape! Expected [1, N, 16]\n");
        }
        
        printf("Final face count: %zu\n", boxes.size());

    } catch (const std::exception& e) {
        printf("Inference error: %s\n", e.what());
        return boxes;
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
        printf("Failed to decode JPEG\n");
        return result;
    }

    cv::Mat frame_detection;
    cv::resize(frame_source, frame_detection, cv::Size(320, 320));

    auto boxes = detectFaces(frame_detection);

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