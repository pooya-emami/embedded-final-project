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

static inline float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

static std::vector<cv::Rect> detectHumans(const cv::Mat &img320)
{
    load_yolo();

    std::vector<cv::Rect> boxes;
    if (!yolo_loaded)
        return boxes;

    // STEP 2: Debug frame dump
    static int dbg_frame_count = 0;
    if (dbg_frame_count++ % 30 == 0) {
        cv::imwrite("/tmp/debug_input.jpg", img320);
        printf("Debug: Dumped frame %d to /tmp/debug_input.jpg\n", dbg_frame_count);
        printf("Debug: img320 size: %dx%d, channels: %d\n", 
               img320.cols, img320.rows, img320.channels());
    }

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

    // Verify input tensor has valid data
    float mean_r = 0.0f, mean_g = 0.0f, mean_b = 0.0f;
    int pixel_count = 320 * 320;
    for (int i = 0; i < pixel_count; i++) {
        mean_r += input_tensor_values[i];
        mean_g += input_tensor_values[pixel_count + i];
        mean_b += input_tensor_values[2 * pixel_count + i];
    }
    mean_r /= pixel_count;
    mean_g /= pixel_count;
    mean_b /= pixel_count;
    printf("Input tensor means - R: %.3f, G: %.3f, B: %.3f (should be ~0.5 for average image)\n", 
           mean_r, mean_g, mean_b);

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

    auto output_tensors = yolo_session->Run(
        Ort::RunOptions{nullptr},
        input_names, &input_tensor, 1,
        output_names, 1);

    float* out = output_tensors[0].GetTensorMutableData<float>();

    auto shape = output_tensors[0]
        .GetTensorTypeAndShapeInfo()
        .GetShape();

    int channels = shape[1];        // 84
    int num_predictions = shape[2]; // 2100

    printf("Model output shape: channels=%d, predictions=%d\n", channels, num_predictions);

    struct Detection {
        float x1, y1, x2, y2;
        float confidence;
        int class_id;
    };
    std::vector<Detection> detections;

    // STEP 1: Track highest score
    float frame_max_score = 0.0f;
    int frame_max_class = -1;
    float frame_max_score_all_classes = 0.0f;
    int frame_max_class_all = -1;

    // Model output: channels 0-3 are cx,cy,w,h in 0-320 pixel space (raw),
    // channels 4-83 are already post-sigmoid class probabilities.
    // No activation should be applied here — same as the working Python version.
    for (int i = 0; i < num_predictions; i++) {
        float x = out[0 * num_predictions + i];
        float y = out[1 * num_predictions + i];
        float w = out[2 * num_predictions + i];
        float h = out[3 * num_predictions + i];

        float best_score = 0.0f;
        int best_class = -1;
        for (int c = 4; c < channels; c++) {
            float s = out[c * num_predictions + i];
            if (s > best_score) {
                best_score = s;
                best_class = c - 4;
            }
        }

        // Track highest score for debugging (all classes)
        if (best_score > frame_max_score_all_classes) {
            frame_max_score_all_classes = best_score;
            frame_max_class_all = best_class;
        }

        // Track highest score for class 0 (person) specifically
        if (best_class == 0 && best_score > frame_max_score) {
            frame_max_score = best_score;
            frame_max_class = best_class;
        }

        if (best_score < 0.25f) continue;
        if (best_class != 0) continue; // person class only

        float x1 = (x - w / 2.0f) * img320.cols / 320.0f;
        float y1 = (y - h / 2.0f) * img320.rows / 320.0f;
        float x2 = (x + w / 2.0f) * img320.cols / 320.0f;
        float y2 = (y + h / 2.0f) * img320.rows / 320.0f;

        detections.push_back({x1, y1, x2, y2, best_score, best_class});
    }

    // STEP 1: Print frame max scores
    printf("Frame max score (class 0 - person): %.4f\n", frame_max_score);
    printf("Frame max score (any class): %.4f (class %d)\n", 
           frame_max_score_all_classes, frame_max_class_all);

    // Additional debug: check if we got any detections at all
    printf("Total predictions processed: %d, detections before NMS: %zu\n", 
           num_predictions, detections.size());

    // Print first few prediction scores for debugging
    printf("First 5 prediction scores (class 0): ");
    for (int i = 0; i < std::min(5, num_predictions); i++) {
        float score = out[4 * num_predictions + i]; // class 0 is at channel 4
        printf("%.4f ", score);
    }
    printf("\n");

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

        printf("NMS returned %zu boxes\n", indices.size());

        for (int idx : indices) {
            boxes.push_back(rects[idx]);
        }
    }

    printf("Final boxes count: %zu\n", boxes.size());
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

    printf("\n=== process_frame called ===\n");
    printf("Input JPEG size: %zu bytes\n", jpeg_len);

    std::vector<uint8_t> buf(jpeg_data, jpeg_data + jpeg_len);
    cv::Mat frame_source = cv::imdecode(buf, cv::IMREAD_COLOR);

    if (frame_source.empty()) {
        printf("ERROR: Failed to decode JPEG\n");
        return result;
    }
    printf("Decoded frame: %dx%d, channels: %d\n", 
           frame_source.cols, frame_source.rows, frame_source.channels());

    // Dump the decoded frame once every 30 frames
    static int debug_count = 0;
    if (debug_count++ % 30 == 0) {
        cv::imwrite("/tmp/decoded_frame.jpg", frame_source);
        printf("Saved decoded frame to /tmp/decoded_frame.jpg\n");
    }

    cv::Mat frame_detection;
    cv::resize(frame_source, frame_detection, cv::Size(320, 320));
    printf("Resized frame to: %dx%d\n", frame_detection.cols, frame_detection.rows);

    auto boxes = detectHumans(frame_detection);
    printf("Detection complete: %zu persons found\n", boxes.size());

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
    printf("Resized output to: %dx%d\n", output_width, output_height);

    std::vector<uint8_t> jpegBuf;
    cv::imencode(".jpg", frame_output, jpegBuf);
    printf("Encoded JPEG size: %zu bytes\n", jpegBuf.size());

    result.jpeg_output = (uint8_t*)malloc(jpegBuf.size());
    if (result.jpeg_output) {
        result.jpeg_length = jpegBuf.size();
        memcpy(result.jpeg_output, jpegBuf.data(), jpegBuf.size());
    }

    result.width = output_width;
    result.height = output_height;
    result.person_count = (int)boxes.size();

    printf("=== process_frame complete ===\n\n");
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