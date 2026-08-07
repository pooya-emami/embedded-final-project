#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <errno.h>

#include "mjpeg_relay.h"
#include "shared_frame.h"

static shared_frame_t *g_frame;

int find_marker(const unsigned char *buf, size_t len, unsigned char m1, unsigned char m2) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == m1 && buf[i+1] == m2)
            return i;
    }
    return -1;
}

void *receiver_thread(void *arg) {
    (void)arg;
    int server_fd, client_fd;
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return NULL;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(RELAY_PORT);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return NULL;
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        return NULL;
    }

    printf("MJPEG relay listening on port %d\n", RELAY_PORT);

    unsigned char buf[RELAY_BUF_SIZE];
    unsigned char jpeg[RELAY_BUF_SIZE];
    size_t jpeg_len = 0;
    int frame_count = 0;

    while (1) {
        client_fd = accept(server_fd, (struct sockaddr*)&addr, &addr_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }
        
        printf("Client connected from %s:%d\n", 
               inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

        jpeg_len = 0;
        frame_count = 0;

        while (1) {
            ssize_t n = read(client_fd, buf, RELAY_BUF_SIZE);
            if (n <= 0) {
                if (n == 0) {
                    printf("Client closed connection\n");
                } else {
                    perror("read");
                }
                break;
            }

            if (jpeg_len + n < RELAY_BUF_SIZE) {
                memcpy(jpeg + jpeg_len, buf, n);
                jpeg_len += n;
            } else {
                printf("Buffer full (%zu bytes), attempting to recover...\n", jpeg_len);
                
                int soi = find_marker(jpeg, jpeg_len, 0xFF, 0xD8);
                int eoi = find_marker(jpeg, jpeg_len, 0xFF, 0xD9);
                
                if (soi >= 0 && eoi > soi) {
                    size_t frame_size = eoi + 2;
                    shared_frame_write(g_frame, jpeg + soi, frame_size - soi);
                    frame_count++;
                    
                    if (frame_size < jpeg_len) {
                        memmove(jpeg, jpeg + frame_size, jpeg_len - frame_size);
                        jpeg_len -= frame_size;
                    } else {
                        jpeg_len = 0;
                    }
                    continue;
                }
                
                int last_soi = -1;
                for (int i = 0; i < (int)jpeg_len - 1; i++) {
                    if (jpeg[i] == 0xFF && jpeg[i+1] == 0xD8) {
                        last_soi = i;
                    }
                }
                
                if (last_soi > 0) {
                    printf("Found last SOI at %d, discarding %d bytes\n", last_soi, last_soi);
                    memmove(jpeg, jpeg + last_soi, jpeg_len - last_soi);
                    jpeg_len -= last_soi;
                } else {
                    printf("No valid SOI found, resetting buffer\n");
                    jpeg_len = 0;
                }
            }

            while (1) {
                int soi = find_marker(jpeg, jpeg_len, 0xFF, 0xD8);
                if (soi < 0) {
                    break;
                }

                int eoi = find_marker(jpeg + soi + 2, jpeg_len - soi - 2, 0xFF, 0xD9);
                if (eoi < 0) {
                    break;
                }

                eoi = eoi + soi + 2;
                size_t frame_size = eoi - soi + 2;
                
                frame_count++;
                
                if (frame_size <= RELAY_BUF_SIZE) {
                    shared_frame_write(g_frame, jpeg + soi, frame_size);
                } else {
                    printf("Frame too large: %zu bytes (max %d)\n", frame_size, RELAY_BUF_SIZE);
                }

                if (eoi + 2 < jpeg_len) {
                    memmove(jpeg, jpeg + eoi + 2, jpeg_len - eoi - 2);
                    jpeg_len -= eoi + 2;
                } else {
                    jpeg_len = 0;
                    break;
                }
            }

            if (jpeg_len > RELAY_BUF_SIZE / 2) {
                int soi = find_marker(jpeg, jpeg_len, 0xFF, 0xD8);
                if (soi > 0) {
                    printf("Cleaning %d bytes of garbage before SOI\n", soi);
                    memmove(jpeg, jpeg + soi, jpeg_len - soi);
                    jpeg_len -= soi;
                } else if (soi < 0) {
                    printf("No SOI found in %zu bytes, clearing buffer\n", jpeg_len);
                    jpeg_len = 0;
                }
            }
        }

        close(client_fd);
        printf("Client disconnected - Total frames: %d\n", frame_count);
    }

    return NULL;
}

int main() {
    printf("Starting MJPEG Relay...\n");
    
    g_frame = shared_frame_open();
    if (!g_frame) {
        fprintf(stderr, "Failed to open shared frame buffer\n");
        return 1;
    }
    printf("Shared frame buffer opened successfully\n");

    pthread_t tid;
    if (pthread_create(&tid, NULL, receiver_thread, NULL) != 0) {
        perror("pthread_create");
        return 1;
    }

    printf("Relay thread started. Press Ctrl+C to stop.\n");
    pthread_join(tid, NULL);
    return 0;
}