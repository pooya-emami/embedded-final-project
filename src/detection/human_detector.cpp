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

    cv::Mat blob = cv::dnn::blobFromImage(
        img320, 1.0/255.0, cv::Size(320, 320),
        cv::Scalar(), true, false
    );

    yolo.setInput(blob);

    // -------------------------------
    // Forward pass with debug prints
    // -------------------------------
    std::cout << "[YOLO] Running forward()...\n";

try {
    std::cout << "[YOLO] Testing simple forward()...\n";

    cv::Mat output = yolo.forward();

    std::cout << "[YOLO] forward() SUCCESS!\n";
    std::cout << "[YOLO] dims = " << output.dims << "\n";

    std::cout << "[YOLO] shape = ";
    for (int i = 0; i < output.dims; i++) {
        std::cout << output.size[i] << " ";
    }
    std::cout << "\n";

} catch (const cv::Exception &e) {
    std::cerr << "[YOLO] forward() FAILED:\n";
    std::cerr << e.what() << "\n";
}

return boxes;  // TEMPORARY
    }

    // -------------------------------
    // Handle 2D output
    // -------------------------------
    else if (out.dims == 2) {
        std::cout << "[YOLO] 2D tensor: " << out.rows << "x" << out.cols << "\n";

        if (out.rows == 84 && out.cols == 2100) {
            std::cout << "[YOLO] Transposing (84x2100) → (2100x84)\n";
            cv::transpose(out, out);
        } else if (out.rows == 2100 && out.cols == 84) {
            std::cout << "[YOLO] Already correct shape (2100x84)\n";
        } else {
            std::cerr << "[YOLO] Unexpected 2D shape\n";
            return boxes;
        }
    }

    else {
        std::cerr << "[YOLO] Unexpected dims: " << out.dims << "\n";
        return boxes;
    }

    // -------------------------------
    // Print first row values (debug)
    // -------------------------------
    std::cout << "[YOLO] First row values: ";
    for (int i = 0; i < 10; i++)
        std::cout << out.at<float>(0, i) << " ";
    std::cout << "\n";

    // -------------------------------
    // Decode detections
    // -------------------------------
    int num = out.rows;
    std::cout << "[YOLO] num detections = " << num << "\n";

    std::vector<cv::Rect> raw_boxes;
    std::vector<float> raw_scores;

    for (int i = 0; i < num; i++) {
        float x = out.at<float>(i, 0);
        float y = out.at<float>(i, 1);
        float w = out.at<float>(i, 2);
        float h = out.at<float>(i, 3);

        float best_score = -1;
        int best_class = -1;

        for (int c = 0; c < 80; c++) {
            float score = out.at<float>(i, 4 + c);
            if (score > best_score) {
                best_score = score;
                best_class = c;
            }
        }

        if (best_score > 0.25f && best_class == 0) {
            float x1 = (x - w/2) * 320;
            float y1 = (y - h/2) * 320;
            float x2 = (x + w/2) * 320;
            float y2 = (y + h/2) * 320;

            raw_boxes.emplace_back((int)x1, (int)y1, (int)(x2-x1), (int)(y2-y1));
            raw_scores.push_back(best_score);
        }
    }

    std::cout << "[YOLO] raw person detections: " << raw_boxes.size() << "\n";

    // NMS
    std::vector<int> keep;
    cv::dnn::NMSBoxes(raw_boxes, raw_scores, 0.25f, 0.45f, keep);

    for (int idx : keep)
        boxes.push_back(raw_boxes[idx]);

    std::cout << "[YOLO] final persons: " << boxes.size() << "\n";

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