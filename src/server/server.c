#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "server.h"
#include "shared_frame.h"

static shared_frame_t *g_frame;
static unsigned char cached_frame[BUFFER_SIZE];
static size_t cached_len = 0;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int running = 1;

/* ---------- Telemetry ---------- */
float read_cpu_temp(void) {
    FILE *fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!fp) return -1;
    int temp;
    fscanf(fp, "%d", &temp);
    fclose(fp);
    return temp / 1000.0;
}

long read_free_mem(void) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;
    char key[32];
    long value;
    while (fscanf(fp, "%s %ld", key, &value) != EOF) {
        if (strcmp(key, "MemAvailable:") == 0) {
            fclose(fp);
            return value;
        }
    }
    fclose(fp);
    return -1;
}

float read_cpu_load(void) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1;
    long user, nice, system, idle;
    fscanf(fp, "cpu %ld %ld %ld %ld", &user, &nice, &system, &idle);
    fclose(fp);

    static long prev_total = 0, prev_idle = 0;
    long total = user + nice + system + idle;
    long diff_total = total - prev_total;
    long diff_idle = idle - prev_idle;
    prev_total = total;
    prev_idle = idle;

    if (diff_total == 0) return 0;
    return (1.0f - ((float)diff_idle / diff_total)) * 100.0f;
}

/* ---------- Load HTML template ---------- */
char *load_html_template(void) {
    FILE *fp = fopen(HTML_PATH, "r");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    fread(buf, 1, len, fp);
    buf[len] = '\0';
    fclose(fp);
    return buf;
}

/* ---------- HTTP response ---------- */
void send_response(int client_fd, const char *status, const char *content_type,
                   const void *data, size_t data_len) {
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n",
        status, content_type, data_len);

    send(client_fd, header, header_len, 0);
    if (data && data_len > 0) {
        send(client_fd, data, data_len, 0);
    }
}

/* ---------- Send HTML page ---------- */
void send_html(int client_fd) {
    char *html = load_html_template();
    if (!html) {
        const char *err = "Template load error";
        send_response(client_fd, "500 Internal Server Error", "text/plain",
                      err, strlen(err));
        return;
    }

    send_response(client_fd, "200 OK", "text/html", html, strlen(html));
    free(html);
}

/* ---------- Send JPEG frame ---------- */
void send_frame(int client_fd) {
    pthread_mutex_lock(&cache_mutex);
    if (cached_len == 0) {
        pthread_mutex_unlock(&cache_mutex);
        send_response(client_fd, "404 Not Found", "text/plain", "No frame", 8);
        return;
    }

    unsigned char *copy = malloc(cached_len);
    memcpy(copy, cached_frame, cached_len);
    size_t len = cached_len;
    pthread_mutex_unlock(&cache_mutex);

    send_response(client_fd, "200 OK", "image/jpeg", copy, len);
    free(copy);
}

/* ---------- Send telemetry ---------- */
void send_telemetry(int client_fd) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"temp\":%.2f,\"mem\":%ld,\"cpu\":%.2f}",
        read_cpu_temp(), read_free_mem(), read_cpu_load());

    send_response(client_fd, "200 OK", "application/json", buf, strlen(buf));
}

/* ---------- Frame updater thread ---------- */
void *frame_updater(void *arg) {
    (void)arg;
    while (running) {
        unsigned char buf[BUFFER_SIZE];
        size_t len = shared_frame_read(g_frame, buf, BUFFER_SIZE);
        if (len > 0) {
            pthread_mutex_lock(&cache_mutex);
            memcpy(cached_frame, buf, len);
            cached_len = len;
            pthread_mutex_unlock(&cache_mutex);
        }
        usleep(50000); // ~20 FPS
    }
    return NULL;
}

/* ---------- Main server loop ---------- */
int main() {
    printf("Starting Security System Server...\n");

    g_frame = shared_frame_open();
    if (!g_frame) {
        fprintf(stderr, "Failed to open shared frame\n");
        return 1;
    }
    printf("Shared frame opened\n");

    pthread_t updater;
    pthread_create(&updater, NULL, frame_updater, NULL);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT_HTTP)
    };

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        return 1;
    }

    printf("HTTP server running on port %d\n", PORT_HTTP);
    printf("Open: http://192.168.137.100:%d/\n", PORT_HTTP);
    printf("Press Ctrl+C to stop\n");

    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;

        char req[1024];
        ssize_t n = read(client_fd, req, sizeof(req) - 1);
        if (n > 0) {
            req[n] = '\0';

            char method[16], path[256];
            if (sscanf(req, "%15s %255s", method, path) == 2) {
                if (strcmp(path, "/") == 0)
                    send_html(client_fd);
                else if (strncmp(path, "/frame", 6) == 0)
                    send_frame(client_fd);
                else if (strcmp(path, "/telemetry") == 0)
                    send_telemetry(client_fd);
                else
                    send_response(client_fd, "404 Not Found", "text/plain", "Not Found", 9);
            }
        }

        close(client_fd);
    }

    close(server_fd);
    printf("Server stopped\n");
    return 0;
}