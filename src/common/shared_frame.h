#ifndef SHARED_FRAME_H
#define SHARED_FRAME_H

#include <stddef.h>

#define SHM_FRAME_NAME     "/guard_mjpeg_frame"
#define SEM_FRAME_NAME     "/guard_mjpeg_lock"
#define SHM_FRAME_BUF_SIZE 65536

typedef struct {
    size_t frame_size;
    unsigned char frame_buffer[SHM_FRAME_BUF_SIZE];
    int relay_enabled;      
    int stream_mode;        
} shared_frame_t;

shared_frame_t *shared_frame_open(void);
void shared_frame_write(shared_frame_t *sf, const unsigned char *data, size_t len);
size_t shared_frame_read(shared_frame_t *sf, unsigned char *buf, size_t max);

#endif