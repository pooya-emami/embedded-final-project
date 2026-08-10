#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>

#include "mjpeg_relay.h"
#include "../common/shared_frame.h"

static shared_frame_t *g_frame = NULL;
static volatile int running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

int find_marker(const unsigned char *buf, size_t len, unsigned char m1, unsigned char m2) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == m1 && buf[i+1] == m2)
            return (int)i;
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

    printf("[RELAY] Listening on port %d\n", RELAY_PORT);

    unsigned char buf[RELAY_BUF_SIZE];
    unsigned char jpeg[RELAY_BUF_SIZE];
    size_t jpeg_len = 0;

    while (running) {
        client_fd = accept(server_fd, (struct sockaddr*)&addr, &addr_len);
        if (client_fd < 0) {
            if (!running) break;
            perror("accept");
            continue;
        }
        
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        printf("[RELAY] Client connected from %s:%d\n", 
               inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

        jpeg_len = 0;

        while (running) {
            ssize_t n = read(client_fd, buf, RELAY_BUF_SIZE);
            
            if (n <= 0) {
                if (n == 0) {
                    printf("[RELAY] Client closed connection\n");
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    printf("[RELAY] Read timeout - reconnecting...\n");
                } else {
                    perror("read");
                }
                break;
            }

            if (jpeg_len + n < RELAY_BUF_SIZE) {
                memcpy(jpeg + jpeg_len, buf, n);
                jpeg_len += n;
            } else {
                printf("[RELAY] Buffer full, resetting\n");
                jpeg_len = 0;
                continue;
            }

            int soi = find_marker(jpeg, jpeg_len, 0xFF, 0xD8);
            int eoi = find_marker(jpeg, jpeg_len, 0xFF, 0xD9);

            if (soi >= 0 && eoi > soi) {
                size_t frame_size = eoi + 2;
                
                if (g_frame && g_frame->relay_enabled) {
                    shared_frame_write(g_frame, jpeg + soi, frame_size - soi);
                }

                if (frame_size < jpeg_len) {
                    memmove(jpeg, jpeg + frame_size, jpeg_len - frame_size);
                    jpeg_len -= frame_size;
                } else {
                    jpeg_len = 0;
                }
            }
        }

        close(client_fd);
        printf("[RELAY] Client disconnected, waiting for new connection...\n");
    }

    close(server_fd);
    return NULL;
}

int main(void) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    printf("[RELAY] Starting MJPEG Relay...\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    g_frame = shared_frame_open();
    if (!g_frame) {
        fprintf(stderr, "[RELAY] Failed to open shared frame buffer\n");
        return 1;
    }
    printf("[RELAY] Shared frame buffer opened successfully\n");

    pthread_t tid;
    if (pthread_create(&tid, NULL, receiver_thread, NULL) != 0) {
        perror("pthread_create");
        return 1;
    }

    printf("[RELAY] Relay thread started. Press Ctrl+C to stop.\n");
    pthread_join(tid, NULL);
    
    printf("[RELAY] Stopped.\n");
    return 0;
}