#ifndef DETECTOR_HPP
#define DETECTOR_HPP

#include <vector>
#include <cstdint>

struct DetectionResult {
    std::vector<uint8_t> jpeg_output; 
    int width;
    int height;
    int person_count;  // Add this
};

DetectionResult process_frame(
    const uint8_t* jpeg_data,
    size_t jpeg_len,
    int output_width,
    int output_height
);

#endif