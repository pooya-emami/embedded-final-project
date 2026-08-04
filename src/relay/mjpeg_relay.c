#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>

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

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return NULL;
    }

    // Allow immediate re-bind after restart (avoids "Address already in use"
    // from a socket still in TIME_WAIT after a previous run).
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

    if (listen(server_fd, 1) < 0) {
        perror("listen");
        close(server_fd);
        return NULL;
    }

    printf("MJPEG relay listening on port %d\n", RELAY_PORT);

    unsigned char buf[RELAY_BUF_SIZE];
    unsigned char jpeg[RELAY_BUF_SIZE];
    size_t jpeg_len = 0;

    while (1) {
        client_fd = accept(server_fd, NULL, NULL);
        printf("Client connected\n");

        while (1) {
            ssize_t n = read(client_fd, buf, RELAY_BUF_SIZE);
            if (n <= 0) break;

            int soi = find_marker(buf, n, 0xFF, 0xD8);
            int eoi = find_marker(buf, n, 0xFF, 0xD9);

            if (soi >= 0) {
                jpeg_len = 0;
            }

            if (jpeg_len + n < RELAY_BUF_SIZE) {
                memcpy(jpeg + jpeg_len, buf, n);
                jpeg_len += n;
            }

            if (eoi >= 0 && jpeg_len > 0) {
                shared_frame_write(g_frame, jpeg, jpeg_len);
                jpeg_len = 0;
            }
        }

        close(client_fd);
        printf("Client disconnected\n");
    }

    return NULL;
}

int main() {
    g_frame = shared_frame_open();
    if (!g_frame) {
        fprintf(stderr, "Failed to open shared frame buffer\n");
        return 1;
    }

    pthread_t tid;
    pthread_create(&tid, NULL, receiver_thread, NULL);
    pthread_join(tid, NULL);
    return 0;
}
