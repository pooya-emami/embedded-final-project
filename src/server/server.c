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
#include <time.h>
#include <stdint.h>

#include "server.h"
#include "shared_frame.h"

#define PORT_HTTP 8080
#define PORT_HTTPS 8443
#define BUFFER_SIZE 65536
#define HTML_PATH "../html/template.html"
#define MAX_HISTORY 5
#define FRAME_SKIP 6

static shared_frame_t *g_frame;
static unsigned char current_frame[BUFFER_SIZE];
static size_t current_len = 0;

static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int running = 1;

static SSL_CTX *ssl_ctx = NULL;

static detection_record_t history[MAX_HISTORY];
static int history_count = 0;
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;

static char *html_cache = NULL;

static float cached_temp = -1;
static long cached_mem = -1;
static float cached_cpu = 0;
static time_t last_telemetry_update = 0;
static pthread_mutex_t telemetry_mutex = PTHREAD_MUTEX_INITIALIZER;

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

    fread(buf, 1, len, fp);
    buf[len] = '\0';
    fclose(fp);
    return buf;
}

static float read_temp(void) {
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return -1;

    int t = 0;
    fscanf(f, "%d", &t);
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
    fscanf(f, "cpu %ld %ld %ld %ld", &u, &n, &s, &i);
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

static void *telemetry_updater(void *arg) {
    (void)arg;
    while (running) {
        float temp = read_temp();
        long mem = read_mem_available();
        float cpu = read_cpu_usage();
        
        pthread_mutex_lock(&telemetry_mutex);
        cached_temp = temp;
        cached_mem = mem;
        cached_cpu = cpu;
        last_telemetry_update = time(NULL);
        pthread_mutex_unlock(&telemetry_mutex);
        
        sleep(2);
    }
    return NULL;
}

void add_history(int count, float temp) {
    pthread_mutex_lock(&history_mutex);
    
    if (history_count >= MAX_HISTORY) {
        memmove(&history[0], &history[1], (MAX_HISTORY - 1) * sizeof(detection_record_t));
        history_count = MAX_HISTORY - 1;
    }
    
    history[history_count].count = count;
    history[history_count].timestamp = time(NULL);
    history[history_count].temp = temp;
    history_count++;
    
    pthread_mutex_unlock(&history_mutex);
}

void *frame_updater(void *arg) {
    (void)arg;

    int raw_counter = 0;

    while (running) {
        unsigned char buf[BUFFER_SIZE];
        size_t len = shared_frame_read(g_frame, buf, BUFFER_SIZE);

        if (len == 0) {
            usleep(20000);
            continue;
        }

        raw_counter++;

        if (raw_counter % FRAME_SKIP != 0)
            continue;

        if (buf[0] == 0xFF && buf[1] == 0xD8) {
            pthread_mutex_lock(&frame_mutex);
            memcpy(current_frame, buf, len);
            current_len = len;
            pthread_mutex_unlock(&frame_mutex);
        }

        usleep(20000);
    }

    return NULL;
}

static void set_socket_timeout(int fd, int seconds) {
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void send_redirect(int fd) {
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

static void handle_https(int fd) {
    SSL *ssl = SSL_new(ssl_ctx);
    SSL_set_fd(ssl, fd);

    if (SSL_accept(ssl) <= 0) {
        SSL_free(ssl);
        close(fd);
        return;
    }

    char req[4096];
    int n = SSL_read(ssl, req, sizeof(req) - 1);
    if (n <= 0) {
        SSL_free(ssl);
        close(fd);
        return;
    }

    req[n] = '\0';

    char method[16], path[256];
    sscanf(req, "%15s %255s", method, path);

    char *q = strchr(path, '?');
    if (q) *q = '\0';

    if (strcmp(path, "/") == 0) {
        if (html_cache) {
            char header[512];
            snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: %zu\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n",
                strlen(html_cache));

            SSL_write(ssl, header, strlen(header));
            SSL_write(ssl, html_cache, strlen(html_cache));
        }
        SSL_free(ssl);
        close(fd);
        return;
    }

    if (strcmp(path, "/stream") == 0 || strcmp(path, "/api/v1/stream") == 0) {
        pthread_mutex_lock(&frame_mutex);
        size_t len = current_len;
        unsigned char *copy = NULL;

        if (len > 0) {
            copy = malloc(len);
            if (copy) memcpy(copy, current_frame, len);
        }
        pthread_mutex_unlock(&frame_mutex);

        if (copy && len > 0) {
            char header[256];
            snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %zu\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n",
                len);
            SSL_write(ssl, header, strlen(header));
            SSL_write(ssl, copy, len);
            free(copy);
        }
        SSL_free(ssl);
        close(fd);
        return;
    }

    if (strcmp(path, "/telemetry") == 0 || strcmp(path, "/api/v1/telemetry") == 0) {
        pthread_mutex_lock(&telemetry_mutex);
        float temp = cached_temp;
        long mem = cached_mem;
        float cpu = cached_cpu;
        pthread_mutex_unlock(&telemetry_mutex);

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
        SSL_free(ssl);
        close(fd);
        return;
    }

    if (strcmp(path, "/api/v1/persons") == 0) {
        int count = (rand() % 4);
        pthread_mutex_lock(&telemetry_mutex);
        float temp = cached_temp;
        pthread_mutex_unlock(&telemetry_mutex);
        
        if (count > 0) {
            add_history(count, temp);
        }

        char json[128];
        snprintf(json, sizeof(json),
                 "{\"count\":%d,\"timestamp\":%ld}",
                 count, time(NULL));

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
        SSL_free(ssl);
        close(fd);
        return;
    }

    if (strcmp(path, "/api/v1/history") == 0) {
        pthread_mutex_lock(&history_mutex);
        
        char json[1024];
        char *p = json;
        p += sprintf(p, "{\"history\":[");
        
        for (int i = 0; i < history_count; i++) {
            p += sprintf(p, "{\"count\":%d,\"timestamp\":%ld,\"temp\":%.2f}",
                         history[i].count, history[i].timestamp, history[i].temp);
            if (i < history_count - 1) {
                p += sprintf(p, ",");
            }
        }
        
        p += sprintf(p, "]}");
        pthread_mutex_unlock(&history_mutex);

        char header[2048];
        snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n%s",
            strlen(json), json);

        SSL_write(ssl, header, strlen(header));
        SSL_free(ssl);
        close(fd);
        return;
    }

    if (strcmp(path, "/api/v1/command") == 0) {
        if (strcmp(method, "POST") == 0) {
            char *body = strstr(req, "\r\n\r\n");
            if (body) {
                body += 4;
                if (strstr(body, "reboot")) {
                    const char *resp = 
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: 40\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "{\"status\":\"success\",\"cmd\":\"reboot\"}";
                    SSL_write(ssl, resp, strlen(resp));
                } else {
                    const char *resp = 
                        "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: 35\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "{\"error\":\"Unknown command\"}";
                    SSL_write(ssl, resp, strlen(resp));
                }
            }
        } else {
            const char *resp = 
                "HTTP/1.1 405 Method Not Allowed\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 36\r\n"
                "Connection: close\r\n"
                "\r\n"
                "{\"error\":\"Method not allowed\"}";
            SSL_write(ssl, resp, strlen(resp));
        }
        SSL_free(ssl);
        close(fd);
        return;
    }

    SSL_write(ssl, "HTTP/1.1 404 Not Found\r\n\r\n", 26);
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

    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);

    return ctx;
}

int main(void) {
    printf("Starting Security Server...\n");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    srand(time(NULL));

    html_cache = load_html();
    if (!html_cache) {
        printf("Warning: Failed to load HTML template\n");
    } else {
        printf("HTML template loaded (%zu bytes)\n", strlen(html_cache));
    }

    g_frame = shared_frame_open();
    if (!g_frame) {
        printf("Failed to open shared frame\n");
        return 1;
    }

    pthread_t updater;
    pthread_create(&updater, NULL, frame_updater, NULL);

    pthread_t telemetry_thread;
    pthread_create(&telemetry_thread, NULL, telemetry_updater, NULL);

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

    if (bind(http_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind http");
        return 1;
    }
    listen(http_fd, 10);
    printf("HTTP on %d (redirects to HTTPS)\n", PORT_HTTP);

    int https_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(https_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr_https = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT_HTTPS)
    };

    if (bind(https_fd, (struct sockaddr *)&addr_https, sizeof(addr_https)) < 0) {
        perror("bind https");
        return 1;
    }
    listen(https_fd, 10);

    printf("HTTPS on %d\n", PORT_HTTPS);
    printf("Open: https://192.168.137.100:8443/\n");
    printf("API endpoints available at /api/v1/*\n");
    printf("Frame skip: 1 out of %d frames (%.1f fps)\n", FRAME_SKIP, 30.0/FRAME_SKIP);

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
                set_socket_timeout(fd, 5);
                send_redirect(fd);
                close(fd);
            }
        }

        if (FD_ISSET(https_fd, &fds)) {
            int fd = accept(https_fd, NULL, NULL);
            if (fd >= 0) {
                set_socket_timeout(fd, 10);
                handle_https(fd);
            }
        }
    }

    close(http_fd);
    close(https_fd);
    SSL_CTX_free(ssl_ctx);
    free(html_cache);

    return 0;
}
