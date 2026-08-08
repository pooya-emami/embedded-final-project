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
    } catch (...) {
        std::cerr << "[YOLO] Failed to load model: " << model_path << "\n";
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

    // Forward pass
    cv::Mat out = yolo.forward();

    // -------------------------------
    // 1. Print actual output shape
    // -------------------------------
    std::cout << "[YOLO] Output dims: " << out.dims << " | sizes: ";
    for (int i = 0; i < out.dims; i++)
        std::cout << out.size[i] << " ";
    std::cout << "\n";

    // -------------------------------
    // 2. Validate shape
    // -------------------------------
    if (out.empty()) {
        std::cerr << "[YOLO] forward() returned empty output\n";
        return boxes;
    }

    if (out.dims != 3) {
        std::cerr << "[YOLO] Unexpected dims: " << out.dims << "\n";
        return boxes;
    }

    int d0 = out.size[0];
    int d1 = out.size[1];
    int d2 = out.size[2];

    // Expected: (1, 84, 2100)
    if (d0 != 1 || d1 != 84) {
        std::cerr << "[YOLO] Unexpected shape: "
                  << d0 << "x" << d1 << "x" << d2 << "\n";
        return boxes;
    }

    // -------------------------------
    // 3. Reshape safely → (84, 2100)
    // -------------------------------
    cv::Mat out2;
    try {
        out2 = out.reshape(1, d1);
    } catch (cv::Exception &e) {
        std::cerr << "[YOLO] reshape failed: " << e.what() << "\n";
        return boxes;
    }

    cv::Mat outT;
    try {
        cv::transpose(out2, outT);
    } catch (cv::Exception &e) {
        std::cerr << "[YOLO] transpose failed: " << e.what() << "\n";
        return boxes;
    }

    int num_preds = outT.rows;

    std::vector<cv::Rect> raw_boxes;
    std::vector<float> raw_scores;

    for (int i = 0; i < num_preds; i++) {
        float x = outT.at<float>(i, 0);
        float y = outT.at<float>(i, 1);
        float w = outT.at<float>(i, 2);
        float h = outT.at<float>(i, 3);

        float best_score = -1;
        int best_class = -1;

        for (int c = 0; c < 80; c++) {
            float score = outT.at<float>(i, 4 + c);
            if (score > best_score) {
                best_score = score;
                best_class = c;
            }
        }

        if (best_score < 0.25f)
            continue;

        if (best_class != 0) 
            continue;

        float x1 = x - w * 0.5f;
        float y1 = y - h * 0.5f;
        float x2 = x + w * 0.5f;
        float y2 = y + h * 0.5f;

        cv::Rect rect(
            (int)x1, (int)y1,
            (int)(x2 - x1),
            (int)(y2 - y1)
        );

        raw_boxes.push_back(rect);
        raw_scores.push_back(best_score);
    }

    std::vector<int> keep;
    cv::dnn::NMSBoxes(raw_boxes, raw_scores,
                      0.25f, 0.45f, keep);

    for (int idx : keep)
        boxes.push_back(raw_boxes[idx]);

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

    // Draw boxes
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
    result.jpeg_length = jpegBuf.size();
    memcpy(result.jpeg_output, jpegBuf.data(), jpegBuf.size());

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
