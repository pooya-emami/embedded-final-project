#ifndef DETECTOR_HPP
#define DETECTOR_HPP

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODEL_BASE_PATH "../../models/"
#define YOLO_MODEL_FILE "yolov5nu.onnx"

typedef struct {
    uint8_t* jpeg_output;   // malloc'ed buffer
    size_t   jpeg_length;   // number of bytes
    int      width;         // output width
    int      height;        // output height
    int      person_count;  // detection count
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

#endif
