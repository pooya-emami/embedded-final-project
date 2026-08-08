#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/dnn.hpp>

#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "human_detector.hpp"

static char model_path[256] = {0};
static cv::dnn::Net yolo;
static bool yolo_loaded = false;

static void load_yolo() {
    if (yolo_loaded) return;
    
    snprintf(model_path, sizeof(model_path),
             "%s%s", MODEL_BASE_PATH, YOLO_MODEL_FILE);

    try {
        yolo = cv::dnn::readNetFromONNX(model_path);
        yolo.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        yolo.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        yolo_loaded = true;
        std::cout << "[YOLO] Loaded model: " << model_path << "\n";
    } catch (const std::exception& e) {
        std::cerr << "[YOLO] Failed to load model: " << model_path << "\n";
        std::cerr << "[YOLO] Error: " << e.what() << "\n";
    }
}

static std::vector<cv::Rect> detectHumans(const cv::Mat &img320) {
    load_yolo();
    std::vector<cv::Rect> boxes;

    if (!yolo_loaded)
        return boxes;

    // Create blob
    cv::Mat blob = cv::dnn::blobFromImage(
        img320, 1.0/255.0, cv::Size(320, 320),
        cv::Scalar(), true, false
    );

    yolo.setInput(blob);

    // Get output - this might return multiple outputs
    std::vector<cv::Mat> outputs;
    std::vector<cv::String> outputNames = yolo.getUnconnectedOutLayersNames();
    yolo.forward(outputs, outputNames);
    
    if (outputs.empty()) {
        std::cerr << "[YOLO] No outputs from network\n";
        return boxes;
    }
    
    // Print all output shapes for debugging
    std::cout << "[YOLO] Number of outputs: " << outputs.size() << "\n";
    for (size_t i = 0; i < outputs.size(); i++) {
        std::cout << "[YOLO] Output " << i << " dims: " << outputs[i].dims << " | sizes: ";
        for (int j = 0; j < outputs[i].dims; j++)
            std::cout << outputs[i].size[j] << " ";
        std::cout << "\n";
    }
    
    // We expect output[0] to be (1, 84, 2100) or (84, 2100)
    cv::Mat out = outputs[0];
    
    // If it's 3D (1, 84, 2100), reshape to (84, 2100)
    if (out.dims == 3) {
        int d0 = out.size[0];
        int d1 = out.size[1];
        int d2 = out.size[2];
        
        std::cout << "[YOLO] Output shape: " << d0 << "x" << d1 << "x" << d2 << "\n";
        
        // Expected: (1, 84, 2100)
        if (d0 == 1 && d1 == 84) {
            // Reshape to (84, 2100) using a different approach
            cv::Mat reshaped = out.reshape(1, d1);  // (84, 2100)
            cv::transpose(reshaped, out);  // (2100, 84)
        } else {
            std::cerr << "[YOLO] Unexpected 3D shape\n";
            return boxes;
        }
    } 
    // If it's 2D, check if it's already (2100, 84)
    else if (out.dims == 2) {
        int rows = out.rows;
        int cols = out.cols;
        std::cout << "[YOLO] Output shape: " << rows << "x" << cols << "\n";
        
        // If it's (84, 2100), transpose to (2100, 84)
        if (rows == 84 && cols == 2100) {
            cv::transpose(out, out);
        }
        // If it's (2100, 84), keep as is
        else if (rows != 2100 || cols != 84) {
            std::cerr << "[YOLO] Unexpected 2D shape: " << rows << "x" << cols << "\n";
            return boxes;
        }
    } else {
        std::cerr << "[YOLO] Unexpected dims: " << out.dims << "\n";
        return boxes;
    }
    
    // Now out should be (num_detections, 84)
    // where num_detections is typically 2100 for 320x320 input
    
    int num_detections = out.rows;
    std::cout << "[YOLO] Number of detections: " << num_detections << "\n";
    
    std::vector<cv::Rect> raw_boxes;
    std::vector<float> raw_scores;

    for (int i = 0; i < num_detections; i++) {
        // Get bbox coordinates (xywh format)
        float x = out.at<float>(i, 0);
        float y = out.at<float>(i, 1);
        float w = out.at<float>(i, 2);
        float h = out.at<float>(i, 3);
        
        // Find best class score (skip first 4 bbox values)
        float best_score = -1.0f;
        int best_class = -1;
        
        for (int c = 0; c < 80; c++) {
            float score = out.at<float>(i, 4 + c);
            if (score > best_score) {
                best_score = score;
                best_class = c;
            }
        }
        
        // Filter: person (class 0) and confidence > 0.25
        if (best_score > 0.25f && best_class == 0) {
            // Convert xywh to xyxy
            float x1 = (x - w/2.0f) * 320.0f;
            float y1 = (y - h/2.0f) * 320.0f;
            float x2 = (x + w/2.0f) * 320.0f;
            float y2 = (y + h/2.0f) * 320.0f;
            
            cv::Rect rect(
                (int)x1, (int)y1,
                (int)(x2 - x1),
                (int)(y2 - y1)
            );
            
            raw_boxes.push_back(rect);
            raw_scores.push_back(best_score);
        }
    }
    
    std::cout << "[YOLO] Detected " << raw_boxes.size() << " persons before NMS\n";

    // Apply NMS
    if (!raw_boxes.empty()) {
        std::vector<int> keep;
        cv::dnn::NMSBoxes(raw_boxes, raw_scores, 0.25f, 0.45f, keep);
        
        for (int idx : keep) {
            boxes.push_back(raw_boxes[idx]);
        }
    }
    
    std::cout << "[YOLO] Final detections: " << boxes.size() << " persons\n";

    return boxes;
}

extern "C" DetectionResult process_frame(
    const uint8_t* jpeg_data,
    size_t jpeg_len,
    int output_width,
    int output_height
) {
    DetectionResult result = {};

    // Decode JPEG
    std::vector<uint8_t> buf(jpeg_data, jpeg_data + jpeg_len);
    cv::Mat frame_source = cv::imdecode(buf, cv::IMREAD_COLOR);

    if (frame_source.empty()) {
        std::cerr << "Failed to decode JPEG frame\n";
        return result;
    }

    // Resize for detection
    cv::Mat frame_detection;
    cv::resize(frame_source, frame_detection, cv::Size(320, 320));

    // Run YOLO
    auto boxes = detectHumans(frame_detection);

    // Draw boxes on the detection image (320x320)
    for (const auto &box : boxes) {
        cv::rectangle(frame_detection, box, cv::Scalar(0, 255, 0), 2);
    }

    // Timestamp
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_time_t), "%Y-%m-%d %H:%M:%S");

    cv::putText(frame_detection, "Student: 404300409",
                cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(0, 255, 255), 2);

    cv::putText(frame_detection, ss.str(),
                cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(255, 255, 255), 2);

    cv::putText(frame_detection,
                "Persons: " + std::to_string(boxes.size()),
                cv::Point(10, 90), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(0, 255, 0), 2);

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

    cv::putText(frame_detection,
                "FPS: " + std::to_string(fps).substr(0, 4),
                cv::Point(frame_detection.cols - 100, 30),
                cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(0, 255, 0), 2);

    // Resize to output resolution
    cv::Mat frame_output;
    cv::resize(frame_detection, frame_output,
               cv::Size(output_width, output_height));

    // Encode JPEG
    std::vector<uint8_t> jpegBuf;
    cv::imencode(".jpg", frame_output, jpegBuf);

    result.jpeg_output = (uint8_t*)malloc(jpegBuf.size());
    if (result.jpeg_output) {
        result.jpeg_length = jpegBuf.size();
        memcpy(result.jpeg_output, jpegBuf.data(), jpegBuf.size());
    }

    result.width = output_width;
    result.height = output_height;
    result.person_count = boxes.size();

    return result;
}

extern "C" void free_detection_result(DetectionResult* res) {
    if (res && res->jpeg_output) {
        free(res->jpeg_output);
        res->jpeg_output = NULL;
        res->jpeg_length = 0;
    }
}