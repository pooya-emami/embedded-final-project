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
static cv::dnn::Net yolo;
static bool yolo_loaded = false;

static void load_yolo()
{
    if (yolo_loaded) return;

    snprintf(model_path, sizeof(model_path), "%s%s", MODEL_BASE_PATH, YOLO_MODEL_FILE);

    yolo = cv::dnn::readNetFromONNX(model_path);
    yolo.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    yolo.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
    yolo_loaded = true;
}

static std::vector<cv::Rect> detectHumans(const cv::Mat &img320)
{
    load_yolo();

    std::vector<cv::Rect> boxes;

    if (!yolo_loaded)
        return boxes;

    cv::Mat blob = cv::dnn::blobFromImage(
        img320,
        1.0 / 255.0,
        cv::Size(320, 320),
        cv::Scalar(),
        true,
        false
    );

    yolo.setInput(blob);

    cv::Mat output = yolo.forward();

    return boxes;
}

extern "C" DetectionResult process_frame(
    const uint8_t* jpeg_data,
    size_t jpeg_len,
    int output_width,
    int output_height)
{
    DetectionResult result = {};

    std::vector<uint8_t> buf(jpeg_data, jpeg_data + jpeg_len);
    cv::Mat frame_source = cv::imdecode(buf, cv::IMREAD_COLOR);

    if (frame_source.empty())
        return result;

    cv::Mat frame_detection;
    cv::resize(frame_source, frame_detection, cv::Size(320, 320));

    auto boxes = detectHumans(frame_detection);

    for (const auto &box : boxes)
        cv::rectangle(frame_detection, box, cv::Scalar(0, 255, 0), 2);

    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(
        std::localtime(&now_time_t),
        "%Y-%m-%d %H:%M:%S"
    );

    cv::putText(
        frame_detection,
        "Student: 404300409",
        cv::Point(10, 30),
        cv::FONT_HERSHEY_SIMPLEX,
        0.6,
        cv::Scalar(0, 255, 255),
        2
    );

    cv::putText(
        frame_detection,
        ss.str(),
        cv::Point(10, 60),
        cv::FONT_HERSHEY_SIMPLEX,
        0.6,
        cv::Scalar(255, 255, 255),
        2
    );

    cv::putText(
        frame_detection,
        "Persons: " + std::to_string(boxes.size()),
        cv::Point(10, 90),
        cv::FONT_HERSHEY_SIMPLEX,
        0.6,
        cv::Scalar(0, 255, 0),
        2
    );

    static double fps = 0;
    static int frame_count = 0;
    static auto start = std::chrono::steady_clock::now();

    auto current = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(
        current - start
    ).count();

    frame_count++;

    if (elapsed >= 1.0) {
        fps = frame_count / elapsed;
        frame_count = 0;
        start = current;
    }

    cv::putText(
        frame_detection,
        "FPS: " + std::to_string(fps).substr(0, 4),
        cv::Point(frame_detection.cols - 100, 30),
        cv::FONT_HERSHEY_SIMPLEX,
        0.6,
        cv::Scalar(0, 255, 0),
        2
    );

    cv::Mat frame_output;

    cv::resize(
        frame_detection,
        frame_output,
        cv::Size(output_width, output_height)
    );

    std::vector<uint8_t> jpegBuf;

    cv::imencode(
        ".jpg",
        frame_output,
        jpegBuf
    );

    result.jpeg_output = (uint8_t*)malloc(jpegBuf.size());

    if (result.jpeg_output) {
        result.jpeg_length = jpegBuf.size();
        memcpy(
            result.jpeg_output,
            jpegBuf.data(),
            jpegBuf.size()
        );
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