#ifndef SHARED_FRAME_PROCESSED_H
#define SHARED_FRAME_PROCESSED_H

#include <stddef.h>
#include <time.h>

#define SHM_PROCESSED_NAME   "/guard_processed_frame"
#define SEM_PROCESSED_NAME   "/guard_processed_lock"
#define SHM_PROCESSED_BUF_SIZE 65536

typedef struct {
    size_t frame_size;
    int person_count;
    float cpu_temp;
    time_t timestamp;
    unsigned char frame_buffer[SHM_PROCESSED_BUF_SIZE];
    int thermal_throttle_active;
    int target_width;
    int target_height;
} processed_frame_t;

processed_frame_t *processed_frame_open(void);
void processed_frame_write(processed_frame_t *pf, const unsigned char *data, size_t len, int count, float temp);
size_t processed_frame_read(processed_frame_t *pf, unsigned char *buf, size_t max, int *count, float *temp);

#endif