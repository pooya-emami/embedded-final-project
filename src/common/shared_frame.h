#ifndef SHARED_FRAME_H
#define SHARED_FRAME_H

#include <stddef.h>

#define SHM_FRAME_NAME     "/guard_mjpeg_frame"
#define SEM_FRAME_NAME     "/guard_mjpeg_lock"
#define SHM_FRAME_BUF_SIZE 65536

typedef struct {
    size_t frame_size;
    unsigned char frame_buffer[SHM_FRAME_BUF_SIZE];
} shared_frame_t;

/* Opens (creating if necessary) the shared frame buffer and its lock.
 * Safe to call from the relay and the server independently, in any
 * start order -- POSIX shm objects are zero-initialized on first
 * creation, and sem_open() on an already-existing semaphore just
 * returns a handle to it without resetting its value.
 * Returns NULL on failure (check errno / stderr for the reason). */
shared_frame_t *shared_frame_open(void);

/* Copies len bytes (clamped to SHM_FRAME_BUF_SIZE) into the shared
 * frame buffer. Called by the relay once a full JPEG frame is ready. */
void shared_frame_write(shared_frame_t *sf, const unsigned char *data, size_t len);

/* Copies up to max bytes of the current frame into buf.
 * Returns the number of bytes copied (0 if no frame yet). */
size_t shared_frame_read(shared_frame_t *sf, unsigned char *buf, size_t max);

#endif /* SHARED_FRAME_H */
