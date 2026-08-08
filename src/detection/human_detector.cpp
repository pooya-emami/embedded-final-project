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

// Softmax over a fixed-size class-score vector. Used here purely as a
// *diagnostic* re-normalization: since the raw per-class sigmoid outputs
// are all sitting in a near-identical tiny range, softmax turns them into
// a relative probability distribution so we can see which class the model
// favors even when no class clears an absolute confidence threshold.
static void softmax(const float* scores_in, float* probs_out, int n)
{
    float max_val = *std::max_element(scores_in, scores_in + n);
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        probs_out[i] = std::exp(scores_in[i] - max_val);
        sum += probs_out[i];
    }
    for (int i = 0; i < n; i++) {
        probs_out[i] /= sum;
    }
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
    int num_classes = channels - 4; // 80

    printf("Model output shape: channels=%d, predictions=%d\n", channels, num_predictions);

    struct Detection {
        float x1, y1, x2, y2;
        float confidence;
        int class_id;
    };
    std::vector<Detection> detections;

    // --- Find the single best-scoring prediction in the whole frame,
    //     across ALL classes, regardless of which class wins. ---
    int best_pred_idx = -1;
    float best_pred_score = -1.0f;
    int best_pred_class = -1;

    float frame_max_score = 0.0f;      // best score specifically for class 0 (person)
    int frame_max_class = -1;

    for (int i = 0; i < num_predictions; i++) {
        for (int c = 4; c < channels; c++) {
            float s = out[c * num_predictions + i];
            if (s > best_pred_score) {
                best_pred_score = s;
                best_pred_idx = i;
                best_pred_class = c - 4;
            }
            if (c - 4 == 0 && s > frame_max_score) {
                frame_max_score = s;
                frame_max_class = 0;
            }
        }
    }

    printf("Frame max score (class 0 - person): %.4f\n", frame_max_score);
    printf("Overall best prediction: idx=%d class=%d raw_score=%.6f\n",
           best_pred_idx, best_pred_class, best_pred_score);

    // --- Softmax the single best prediction's class-score vector to get
    //     relative certainty, since absolute sigmoid scores here are all
    //     tiny and non-discriminative. ---
    if (best_pred_idx >= 0) {
        std::vector<float> class_scores(num_classes);
        for (int c = 0; c < num_classes; c++) {
            class_scores[c] = out[(c + 4) * num_predictions + best_pred_idx];
        }

        std::vector<float> probs(num_classes);
        softmax(class_scores.data(), probs.data(), num_classes);

        // argmax after softmax (same argmax as raw scores, softmax is
        // monotonic, but computed explicitly for clarity)
        int softmax_argmax = (int)(std::max_element(probs.begin(), probs.end()) - probs.begin());
        float person_prob = probs[0];
        float top_prob = probs[softmax_argmax];

        printf("Softmax over best prediction's classes: argmax=%d (prob=%.4f), person(class 0) prob=%.4f\n",
               softmax_argmax, top_prob, person_prob);

        bool person_is_argmax = (softmax_argmax == 0);
        printf("Person is argmax for best prediction: %s\n", person_is_argmax ? "YES" : "no");

        // If person wins the argmax competition for the single most
        // confident prediction in the frame, treat that as a detection
        // signal even though the raw sigmoid magnitude is tiny -- this
        // replaces the flat 0.25 absolute-confidence gate for this
        // diagnostic pass.
        if (person_is_argmax) {
            float x = out[0 * num_predictions + best_pred_idx];
            float y = out[1 * num_predictions + best_pred_idx];
            float w = out[2 * num_predictions + best_pred_idx];
            float h = out[3 * num_predictions + best_pred_idx];

            float x1 = (x - w / 2.0f) * img320.cols / 320.0f;
            float y1 = (y - h / 2.0f) * img320.rows / 320.0f;
            float x2 = (x + w / 2.0f) * img320.cols / 320.0f;
            float y2 = (y + h / 2.0f) * img320.rows / 320.0f;

            detections.push_back({x1, y1, x2, y2, best_pred_score, 0});
        }
    }

    printf("Total predictions processed: %d, detections before NMS: %zu\n",
           num_predictions, detections.size());

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
        // score_threshold set to 0 here since we already gated on
        // "person is argmax" above rather than an absolute confidence cut
        cv::dnn::NMSBoxes(rects, scores, 0.0f, 0.45f, indices);

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

    for (const auto &box : boxes)
        cv::rectangle(frame_detection, box, cv::Scalar(0, 255, 0), 2);

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