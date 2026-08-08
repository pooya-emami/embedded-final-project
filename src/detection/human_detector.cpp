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

    cv::Mat blob = cv::dnn::blobFromImage(
        img320, 1/255.0, cv::Size(320, 320),
        cv::Scalar(), true, false
    );

    yolo.setInput(blob);

    cv::Mat out = yolo.forward();  // shape: (1, 84, 2100)

    cv::Mat out2;
    cv::transpose(out.reshape(1, 84), out2);

    cv::Mat boxes_mat = out2.colRange(0, 4);      // (2100, 4)
    cv::Mat scores_mat = out2.colRange(4, 84);    // (2100, 80)

    std::vector<cv::Vec4f> raw_boxes;
    std::vector<float> confidences;
    std::vector<int> class_ids;

    for (int i = 0; i < boxes_mat.rows; i++) {
        cv::Vec4f b = boxes_mat.row(i);

        cv::Mat score_row = scores_mat.row(i);

        cv::Point classIdPoint;
        double maxScore;
        cv::minMaxLoc(score_row, nullptr, &maxScore, nullptr, &classIdPoint);

        if (maxScore > 0.25f) {
            raw_boxes.push_back(b);
            confidences.push_back((float)maxScore);
            class_ids.push_back(classIdPoint.x);
        }
    }

    if (raw_boxes.empty())
        return boxes;

    std::vector<cv::Rect> boxes_xyxy;
    boxes_xyxy.reserve(raw_boxes.size());

    for (auto &b : raw_boxes) {
        float x = b[0];
        float y = b[1];
        float w = b[2];
        float h = b[3];

        float x1 = x - w/2.0f;
        float y1 = y - h/2.0f;
        float x2 = x + w/2.0f;
        float y2 = y + h/2.0f;

        boxes_xyxy.emplace_back(
            cv::Point((int)x1, (int)y1),
            cv::Point((int)x2, (int)y2)
        );
    }

    float scale_x = img320.cols / 320.0f;
    float scale_y = img320.rows / 320.0f;

    for (auto &r : boxes_xyxy) {
        r.x *= scale_x;
        r.y *= scale_y;
        r.width *= scale_x;
        r.height *= scale_y;
    }

    std::vector<int> keep;
    cv::dnn::NMSBoxes(boxes_xyxy, confidences, 0.25f, 0.45f, keep);

    for (int idx : keep) {
        if (class_ids[idx] == 0)
            boxes.push_back(boxes_xyxy[idx]);
    }

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
