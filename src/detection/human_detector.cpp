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
static Ort::Env ort_env(ORT_LOGGING_LEVEL_WARNING, "BlazeFace");
static Ort::Session* blaze_session = nullptr;
static bool blaze_loaded = false;

// BlazeFace constants
const int INPUT_SIZE = 128;
const float CONF_THRESH = 0.5f;
const float IOU_THRESH = 0.3f;
const int MAX_DETECTIONS = 25;

static void load_model()
{
    if (blaze_loaded)
        return;

    snprintf(model_path, sizeof(model_path),
             "%s%s", MODEL_BASE_PATH, YOLO_MODEL_FILE);

    printf("Loading BlazeFace model from: %s\n", model_path);

    Ort::SessionOptions session_options;
    session_options.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_ALL
    );
    session_options.SetIntraOpNumThreads(4);
    session_options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);

    try {
        blaze_session = new Ort::Session(
            ort_env,
            model_path,
            session_options
        );
        blaze_loaded = true;
        printf("Model loaded successfully!\n");
    } catch (const Ort::Exception& e) {
        printf("Error loading model: %s\n", e.what());
        blaze_loaded = false;
    }
}

static std::vector<cv::Rect> detectFaces(const cv::Mat &img320)
{
    load_model();

    std::vector<cv::Rect> boxes;
    if (!blaze_loaded)
        return boxes;

    // Convert to 128x128 for BlazeFace
    cv::Mat img128;
    cv::resize(img320, img128, cv::Size(INPUT_SIZE, INPUT_SIZE));
    
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

    // Create input tensors in the exact order expected by the model
    std::vector<const char*> input_names = {"image", "conf_threshold", "max_detections", "iou_threshold"};
    std::vector<Ort::Value> input_tensors;

    // 1. Image tensor
    Ort::Value image_tensor = Ort::Value::CreateTensor<float>(
        mem_info,
        input_tensor_values.data(),
        input_tensor_values.size(),
        input_shape.data(),
        input_shape.size()
    );
    input_tensors.push_back(std::move(image_tensor));
    
    // 2. conf_threshold
    float conf_thresh = CONF_THRESH;
    std::array<int64_t, 1> conf_shape = {1};
    Ort::Value conf_tensor = Ort::Value::CreateTensor<float>(
        mem_info,
        &conf_thresh,
        1,
        conf_shape.data(),
        conf_shape.size()
    );
    input_tensors.push_back(std::move(conf_tensor));
    
    // 3. max_detections
    int64_t max_det = MAX_DETECTIONS;
    std::array<int64_t, 1> max_shape = {1};
    Ort::Value max_tensor = Ort::Value::CreateTensor<int64_t>(
        mem_info,
        &max_det,
        1,
        max_shape.data(),
        max_shape.size()
    );
    input_tensors.push_back(std::move(max_tensor));
    
    // 4. iou_threshold
    float iou_thresh = IOU_THRESH;
    std::array<int64_t, 1> iou_shape = {1};
    Ort::Value iou_tensor = Ort::Value::CreateTensor<float>(
        mem_info,
        &iou_thresh,
        1,
        iou_shape.data(),
        iou_shape.size()
    );
    input_tensors.push_back(std::move(iou_tensor));

    // Output names
    const char* output_names[] = {"selectedBoxes"};

    // Run inference
    std::vector<Ort::Value> output_tensors;
    try {
        output_tensors = blaze_session->Run(
            Ort::RunOptions{nullptr},
            input_names.data(), input_tensors.data(), input_tensors.size(),
            output_names, 1);
    } catch (const Ort::Exception& e) {
        printf("Inference error: %s\n", e.what());
        return boxes;
    }

    if (output_tensors.empty()) {
        return boxes;
    }

    // Parse output - shape is [1, 896, 16]
    float* boxes_data = output_tensors[0].GetTensorMutableData<float>();
    auto boxes_shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();
    
    // Shape should be [1, 896, 16]
    if (boxes_shape.size() != 3 || boxes_shape[0] != 1) {
        printf("Unexpected output shape\n");
        return boxes;
    }

    int num_predictions = boxes_shape[1]; // 896
    int num_values = boxes_shape[2];      // 16

    // Parse detections
    struct Detection {
        float x1, y1, x2, y2;
        float confidence;
    };
    std::vector<Detection> detections;

    // Each detection has 16 values: [y1, x1, y2, x2, landmarks...]
    // The first 4 are the box coordinates, and value at index 15 is the confidence
    for (int i = 0; i < num_predictions; i++) {
        int offset = i * num_values;
        
        float y1 = boxes_data[offset + 0];
        float x1 = boxes_data[offset + 1];
        float y2 = boxes_data[offset + 2];
        float x2 = boxes_data[offset + 3];
        
        // Confidence is at index 15 (last value)
        float score = boxes_data[offset + 15];
        
        // Apply confidence threshold
        if (score < CONF_THRESH) continue;
        
        // Validate detection
        if (x1 < 0 || x1 > 1 || x2 < 0 || x2 > 1 ||
            y1 < 0 || y1 > 1 || y2 < 0 || y2 > 1 ||
            x2 <= x1 || y2 <= y1) {
            continue;
        }
        
        // Convert from normalized to pixel coordinates (320x320)
        float px1 = x1 * img320.cols;
        float py1 = y1 * img320.rows;
        float px2 = x2 * img320.cols;
        float py2 = y2 * img320.rows;
        
        detections.push_back({px1, py1, px2, py2, score});
    }

    // Apply NMS
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
        cv::dnn::NMSBoxes(rects, scores, CONF_THRESH, IOU_THRESH, indices);
        
        for (int idx : indices) {
            boxes.push_back(rects[idx]);
        }
    }

    return boxes;
}

// Keep the function name for compatibility
static std::vector<cv::Rect> detectHumans(const cv::Mat &img320)
{
    return detectFaces(img320);
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
        return result;
    }

    cv::Mat frame_detection;
    cv::resize(frame_source, frame_detection, cv::Size(320, 320));

    auto boxes = detectHumans(frame_detection);

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