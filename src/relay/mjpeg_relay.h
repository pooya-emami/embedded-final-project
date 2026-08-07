#ifndef MJPEG_RELAY_H
#define MJPEG_RELAY_H

#include <stddef.h>

#define RELAY_PORT     9000
#define RELAY_BUF_SIZE 262144

int find_marker(const unsigned char *buf, size_t len, unsigned char m1, unsigned char m2);
void *receiver_thread(void *arg);

#endif /* MJPEG_RELAY_H */