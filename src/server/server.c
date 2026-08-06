#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "shared_frame.h"

#define PORT_HTTP 8080
#define PORT_HTTPS 8443
#define BUFFER_SIZE 65536
#define HTML_PATH "../html/template.html"

static shared_frame_t *g_frame;
static unsigned char current_frame[BUFFER_SIZE];
static size_t current_len = 0;

static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int running = 1;

static SSL_CTX *ssl_ctx = NULL;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

static char *load_html(void) {
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

    size_t r = fread(buf, 1, len, fp);
    (void)r;
    buf[len] = '\0';
    fclose(fp);
    return buf;
}

static float read_temp(void) {
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return -1;

    int t = 0;
    if (fscanf(f, "%d", &t) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return t / 1000.0f;
}

static long read_mem_available(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;

    char key[32];
    long val = -1;

    while (fscanf(f, "%31s %ld", key, &val) != EOF) {
        if (strcmp(key, "MemAvailable:") == 0) {
            fclose(f);
            return val;
        }
    }

    fclose(f);
    return -1;
}

static float read_cpu_usage(void) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;

    long u, n, s, i;
    if (fscanf(f, "cpu %ld %ld %ld %ld", &u, &n, &s, &i) != 4) {
        fclose(f);
        return 0;
    }
    fclose(f);

    static long prev_total = 0;
    static long prev_idle = 0;

    long total = u + n + s + i;
    long diff_total = total - prev_total;
    long diff_idle = i - prev_idle;

    prev_total = total;
    prev_idle = i;

    if (diff_total <= 0) return 0;
    return (1.0f - ((float)diff_idle / diff_total)) * 100.0f;
}

static void *frame_updater(void *arg) {
    (void)arg;

    while (running) {
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

static void send_response(int fd, const char *status,
                          const char *type,
                          const void *data,
                          size_t len)
{
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

    if (data && len > 0)
        send(fd, data, len, 0);
}

static void send_redirect(int fd) {
    const char *msg =
        "HTTP/1.1 301 Moved Permanently\r\n"
        "Location: https://192.168.137.100:8443/\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 24\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html>Redirecting...</html>";

    send(fd, msg, strlen(msg), 0);
}

__attribute__((unused))
static void handle_request(int fd) {
    char req[1024];
    int n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) {
        close(fd);
        return;
    }

    req[n] = '\0';

    char path[256];
    sscanf(req, "%*s %255s", path);

    char *q = strchr(path, '?');
    if (q) *q = '\0';

    if (strcmp(path, "/") == 0) {
        char *html = load_html();
        if (!html) {
            send_response(fd, "500 Error", "text/plain", "Template error", 14);
        } else {
            send_response(fd, "200 OK", "text/html", html, strlen(html));
            free(html);
        }
    }
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
    else if (strcmp(path, "/telemetry") == 0) {
        float temp = read_temp();
        long mem = read_mem_available();
        float cpu = read_cpu_usage();

        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"temp\":%.2f,\"mem\":%ld,\"cpu\":%.2f}",
                 temp, mem, cpu);

        send_response(fd, "200 OK", "application/json", buf, strlen(buf));
    }
    else {
        send_response(fd, "404 Not Found", "text/plain", "Not Found", 9);
    }

    close(fd);
}

static void handle_https(int fd) {
    SSL *ssl = SSL_new(ssl_ctx);
    SSL_set_fd(ssl, fd);

    if (SSL_accept(ssl) <= 0) {
        SSL_free(ssl);
        close(fd);
        return;
    }

    char req[1024];
    int n = SSL_read(ssl, req, sizeof(req) - 1);
    if (n <= 0) {
        SSL_free(ssl);
        close(fd);
        return;
    }

    req[n] = '\0';

    char path[256];
    sscanf(req, "%*s %255s", path);

    char *q = strchr(path, '?');
    if (q) *q = '\0';

    if (strcmp(path, "/") == 0) {
        char *html = load_html();
        if (html) {
            char header[512];
            snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: %zu\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n",
                strlen(html));

            SSL_write(ssl, header, strlen(header));
            SSL_write(ssl, html, strlen(html));
            free(html);
        }
    }

    else if (strcmp(path, "/stream") == 0) {
        pthread_mutex_lock(&frame_mutex);

        if (current_len > 0) {
            char header[256];
            snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %zu\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n",
                current_len);

            SSL_write(ssl, header, strlen(header));
            SSL_write(ssl, current_frame, current_len);
        }

        pthread_mutex_unlock(&frame_mutex);
    }

    else if (strcmp(path, "/telemetry") == 0) {
        float temp = read_temp();
        long mem = read_mem_available();
        float cpu = read_cpu_usage();

        char json[256];
        snprintf(json, sizeof(json),
                 "{\"temp\":%.2f,\"mem\":%ld,\"cpu\":%.2f}",
                 temp, mem, cpu);

        char header[512];
        snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n%s",
            strlen(json), json);

        SSL_write(ssl, header, strlen(header));
    }
    else {
        SSL_write(ssl, "HTTP/1.1 404 Not Found\r\n\r\n", 26);
    }

    SSL_free(ssl);
    close(fd);
}

static SSL_CTX *init_ssl(void) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return NULL;

    if (SSL_CTX_use_certificate_file(ctx, "cert.pem", SSL_FILETYPE_PEM) <= 0)
        return NULL;

    if (SSL_CTX_use_PrivateKey_file(ctx, "key.pem", SSL_FILETYPE_PEM) <= 0)
        return NULL;

    return ctx;
}

int main(void) {
    printf("Starting Security Server...\n");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    g_frame = shared_frame_open();
    if (!g_frame) {
        printf("Failed to open shared frame\n");
        return 1;
    }

    pthread_t updater;
    pthread_create(&updater, NULL, frame_updater, NULL);

    ssl_ctx = init_ssl();
    if (!ssl_ctx) {
        printf("SSL init failed. Generate cert.pem and key.pem\n");
        return 1;
    }

    printf("SSL initialized\n");

    int http_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(http_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT_HTTP)
    };

    bind(http_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(http_fd, 10);

    printf("HTTP on %d (redirects to HTTPS)\n", PORT_HTTP);

    int https_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(https_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr_https = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT_HTTPS)
    };

    bind(https_fd, (struct sockaddr *)&addr_https, sizeof(addr_https));
    listen(https_fd, 10);

    printf("HTTPS on %d\n", PORT_HTTPS);
    printf("Open: https://192.168.137.100:8443/\n");

    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(http_fd, &fds);
        FD_SET(https_fd, &fds);

        int max_fd = (https_fd > http_fd) ? https_fd : http_fd;
        select(max_fd + 1, &fds, NULL, NULL, NULL);

        if (FD_ISSET(http_fd, &fds)) {
            int fd = accept(http_fd, NULL, NULL);
            if (fd >= 0) {
                send_redirect(fd);
                close(fd);
            }
        }

        if (FD_ISSET(https_fd, &fds)) {
            int fd = accept(https_fd, NULL, NULL);
            if (fd >= 0)
                handle_https(fd);
        }
    }

    close(http_fd);
    close(https_fd);
    SSL_CTX_free(ssl_ctx);

    return 0;
}
