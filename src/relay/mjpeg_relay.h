#ifndef MJPEG_RELAY_H
#define MJPEG_RELAY_H

#include <stddef.h>

#define RELAY_PORT     9000
#define RELAY_BUF_SIZE 65536

/* Scans buf[0..len) for the two-byte marker m1,m2.
 * Returns the byte offset of m1 if found, -1 otherwise. */
int find_marker(const unsigned char *buf, size_t len,
                 unsigned char m1, unsigned char m2);

/* Thread entry point: accepts MJPEG source connections one at a time
 * and reassembles complete JPEG frames into the shared frame buffer. */
void *receiver_thread(void *arg);

#endif /* MJPEG_RELAY_H */
