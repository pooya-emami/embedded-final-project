#include "detector.hpp"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cstdlib>

// Dummy detection - replace with YOLO later
static std::vector<cv::Rect> detectHumans(const cv::Mat &img320) {
    return { cv::Rect(80, 80, 160, 160) };  // fake box
}

extern "C" DetectionResult process_frame(
    const uint8_t* jpeg_data,
    size_t jpeg_len,
    int output_width,
    int output_height
) {
    DetectionResult result = {0};

    // Step 1: Decode JPEG → Mat
    std::vector<uint8_t> buf(jpeg_data, jpeg_data + jpeg_len);
    cv::Mat frame_source = cv::imdecode(buf, cv::IMREAD_COLOR);

    if (frame_source.empty()) {
        std::cerr << "Failed to decode JPEG frame\n";
        return result;
    }

    // Step 2: Resize to 320x320 for detection
    cv::Mat frame_detection;
    cv::resize(frame_source, frame_detection, cv::Size(320, 320));

    // Step 3: Dummy detection
    auto boxes = detectHumans(frame_detection);

    // Step 4: Draw boxes
    for (const auto &box : boxes) {
        cv::rectangle(frame_detection, box, cv::Scalar(0, 255, 0), 2);
    }

    // Step 5: Overlays (student ID, timestamp, FPS)
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

    // Step 6: Resize to output resolution
    cv::Mat frame_output;
    cv::resize(frame_detection, frame_output,
               cv::Size(output_width, output_height));

    // Step 7: Encode to JPEG
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
