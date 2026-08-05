#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "shared_frame.h"

#define PORT 8080
#define BUFFER_SIZE 65536
#define HTML_PATH "../html/template.html"

static shared_frame_t *g_frame;
static unsigned char current_frame[BUFFER_SIZE];
static size_t current_len = 0;
static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;

char *load_html() {
    FILE *fp = fopen(HTML_PATH, "r");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    fread(buf, 1, len, fp);
    buf[len] = '\0';
    fclose(fp);
    return buf;
}

void *frame_updater(void *arg) {
    (void)arg;
    while (1) {
        unsigned char buf[BUFFER_SIZE];
        size_t len = shared_frame_read(g_frame, buf, BUFFER_SIZE);
        if (len > 0 && buf[0] == 0xFF && buf[1] == 0xD8) {
            pthread_mutex_lock(&frame_mutex);
            memcpy(current_frame, buf, len);
            current_len = len;
            pthread_mutex_unlock(&frame_mutex);
        }
        usleep(50000);
    }
    return NULL;
}

void send_response(int fd, const char *status, const char *type, const void *data, size_t len) {
    char header[256];
    snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n",
        status, type, len);
    send(fd, header, strlen(header), 0);
    if (data && len > 0) send(fd, data, len, 0);
}

void handle_request(int fd) {
    char req[1024];
    int n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) { close(fd); return; }
    req[n] = '\0';
    
    char path[256];
    sscanf(req, "%*s %255s", path);
    
    // Remove query string (?t=12345)
    char *q = strchr(path, '?');
    if (q) *q = '\0';
    
    // HTML page
    if (strcmp(path, "/") == 0) {
        char *html = load_html();
        if (!html) {
            send_response(fd, "500 Error", "text/plain", "Template error", 14);
        } else {
            send_response(fd, "200 OK", "text/html", html, strlen(html));
            free(html);
        }
    }
    // Frame (for /frame)
    else if (strcmp(path, "/frame") == 0) {
        pthread_mutex_lock(&frame_mutex);
        if (current_len == 0) {
            pthread_mutex_unlock(&frame_mutex);
            send_response(fd, "404 Not Found", "text/plain", "No frame", 8);
        } else {
            unsigned char *copy = malloc(current_len);
            memcpy(copy, current_frame, current_len);
            size_t len = current_len;
            pthread_mutex_unlock(&frame_mutex);
            send_response(fd, "200 OK", "image/jpeg", copy, len);
            free(copy);
        }
    }
    // Stream (for /stream)
    else if (strcmp(path, "/stream") == 0) {
        pthread_mutex_lock(&frame_mutex);
        if (current_len == 0) {
            pthread_mutex_unlock(&frame_mutex);
            send_response(fd, "404 Not Found", "text/plain", "No frame", 8);
        } else {
            unsigned char *copy = malloc(current_len);
            memcpy(copy, current_frame, current_len);
            size_t len = current_len;
            pthread_mutex_unlock(&frame_mutex);
            send_response(fd, "200 OK", "image/jpeg", copy, len);
            free(copy);
        }
    }
    // Telemetry
    else if (strcmp(path, "/telemetry") == 0) {
        float temp = -1;
        FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
        if (f) { int t; fscanf(f, "%d", &t); temp = t/1000.0; fclose(f); }
        
        long mem = -1;
        f = fopen("/proc/meminfo", "r");
        if (f) { char k[32]; long v; while(fscanf(f, "%s %ld", k, &v)!=EOF){if(strcmp(k,"MemAvailable:")==0){mem=v;break;}} fclose(f); }
        
        float cpu = 0;
        f = fopen("/proc/stat", "r");
        if (f) { long u,n,s,i; fscanf(f, "cpu %ld %ld %ld %ld", &u,&n,&s,&i); fclose(f); static long pt=0,pi=0; long t=u+n+s+i; long dt=t-pt, di=i-pi; pt=t; pi=i; if(dt>0) cpu=(1.0f-((float)di/dt))*100.0f; }
        
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"temp\":%.2f,\"mem\":%ld,\"cpu\":%.2f}", temp, mem, cpu);
        send_response(fd, "200 OK", "application/json", buf, strlen(buf));
    }
    else {
        send_response(fd, "404 Not Found", "text/plain", "Not Found", 9);
    }
    close(fd);
}

int main() {
    printf("Starting Security Server...\n");
    
    g_frame = shared_frame_open();
    if (!g_frame) { printf("Failed to open shared frame\n"); return 1; }
    
    pthread_t updater;
    pthread_create(&updater, NULL, frame_updater, NULL);
    
    int server = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr = {.sin_family=AF_INET, .sin_addr.s_addr=INADDR_ANY, .sin_port=htons(PORT)};
    bind(server, (struct sockaddr*)&addr, sizeof(addr));
    listen(server, 10);
    
    printf("Server running on http://192.168.137.100:8080/\n");
    printf("Press Ctrl+C to stop\n");
    
    while (1) {
        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        int fd = accept(server, (struct sockaddr*)&client, &len);
        if (fd >= 0) handle_request(fd);
    }
    return 0;
}