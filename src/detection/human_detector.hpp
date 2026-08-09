#ifndef HUMAN_DETECTOR_HPP
#define HUMAN_DETECTOR_HPP

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODEL_BASE_PATH "../../models/"
#define YOLO_MODEL_FILE "yolov8n.onnx"

typedef struct DetectionResult {
    uint8_t* jpeg_output;
    size_t jpeg_length;
    int width;
    int height;
    int person_count;
} DetectionResult;


DetectionResult process_frame(
    const uint8_t* jpeg_data,
    size_t jpeg_len,
    int output_width,
    int output_height
);


void free_detection_result(DetectionResult* res);

#ifdef __cplusplus
}
#endif

#endif // HUMAN_DETECTOR_HPP