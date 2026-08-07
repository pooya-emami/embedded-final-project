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

// Dummy detection - replace with YOLO later
static std::vector<cv::Rect> detectHumans(const cv::Mat &img320) {
    (void)img320;
    // Simulate detection with some randomness
    static int frame_counter = 0;
    frame_counter++;
    
    // Return random number of people (0-3) for demo
    int count = (frame_counter % 10 < 6) ? (frame_counter % 4) : 0;
    
    std::vector<cv::Rect> boxes;
    for (int i = 0; i < count; i++) {
        int x = 40 + i * 60 + (frame_counter % 20);
        int y = 40 + i * 40 + (frame_counter % 15);
        boxes.push_back(cv::Rect(x, y, 100, 140));
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
    result.jpeg_output = NULL;
    result.jpeg_length = 0;
    result.width = output_width;
    result.height = output_height;
    result.person_count = 0;

    // Step 1: Decode JPEG → Mat
    std::vector<uint8_t> buf(jpeg_data, jpeg_data + jpeg_len);
    cv::Mat frame_source = cv::imdecode(buf, cv::IMREAD_COLOR);

    if (frame_source.empty()) {
        std::cerr << "Failed to decode JPEG frame\n";
        return result;
    }

    // Step 2: Resize to 320x320 for detection (faster processing)
    cv::Mat frame_detection;
    cv::resize(frame_source, frame_detection, cv::Size(320, 320));

    // Step 3: Detection
    auto boxes = detectHumans(frame_detection);

    // Step 4: Draw boxes
    for (const auto &box : boxes) {
        cv::rectangle(frame_detection, box, cv::Scalar(0, 255, 0), 2);
        // Add label
        cv::putText(frame_detection, "Person",
                    cv::Point(box.x, box.y - 5),
                    cv::FONT_HERSHEY_SIMPLEX, 0.4,
                    cv::Scalar(0, 255, 0), 1);
    }

    // Step 5: Overlays (student ID, timestamp, FPS)
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_time_t), "%Y-%m-%d %H:%M:%S");

    // Student ID overlay
    cv::putText(frame_detection, "ID: 404300409",
                cv::Point(10, 25), cv::FONT_HERSHEY_SIMPLEX, 0.5,
                cv::Scalar(0, 255, 255), 2);

    // Timestamp
    cv::putText(frame_detection, ss.str(),
                cv::Point(10, 50), cv::FONT_HERSHEY_SIMPLEX, 0.5,
                cv::Scalar(255, 255, 255), 2);

    // Person count with background
    std::string count_text = "Persons: " + std::to_string(boxes.size());
    cv::rectangle(frame_detection, cv::Point(10, 65), cv::Point(180, 95), cv::Scalar(0, 0, 0), -1);
    cv::putText(frame_detection, count_text,
                cv::Point(15, 88), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(0, 255, 0), 2);

    // FPS calculation
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

    std::string fps_text = "FPS: " + std::to_string(fps).substr(0, 4);
    cv::rectangle(frame_detection, 
                  cv::Point(frame_detection.cols - 120, 5),
                  cv::Point(frame_detection.cols - 10, 30),
                  cv::Scalar(0, 0, 0), -1);
    cv::putText(frame_detection, fps_text,
                cv::Point(frame_detection.cols - 115, 22),
                cv::FONT_HERSHEY_SIMPLEX, 0.5,
                cv::Scalar(0, 255, 0), 2);

    // Step 6: Resize to output resolution
    cv::Mat frame_output;
    cv::resize(frame_detection, frame_output,
               cv::Size(output_width, output_height));

    // Step 7: Encode to JPEG with optimized quality
    std::vector<uint8_t> jpegBuf;
    std::vector<int> params;
    params.push_back(cv::IMWRITE_JPEG_QUALITY);
    params.push_back(75);  // Quality 75 for good balance
    
    cv::imencode(".jpg", frame_output, jpegBuf, params);

    // Allocate and copy result
    result.jpeg_output = (uint8_t*)malloc(jpegBuf.size());
    if (result.jpeg_output) {
        result.jpeg_length = jpegBuf.size();
        memcpy(result.jpeg_output, jpegBuf.data(), jpegBuf.size());
        result.person_count = boxes.size();
    }

    return result;
}

extern "C" void free_detection_result(DetectionResult* res) {
    if (res && res->jpeg_output) {
        free(res->jpeg_output);
        res->jpeg_output = NULL;
        res->jpeg_length = 0;
    }
}