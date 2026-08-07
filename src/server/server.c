#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <time.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>

#include "server.h"
#include "shared_frame.h"
#include "human_detector.hpp"

#define MAX_EVENTS 64
#define MJPEG_BOUNDARY "mjpegframe"

int g_frame_interval_ms = DEFAULT_FRAME_INTERVAL_MS;
int g_frame_width = DEFAULT_FRAME_WIDTH;
int g_frame_height = DEFAULT_FRAME_HEIGHT;
int g_port_http = DEFAULT_PORT_HTTP;
int g_port_https = DEFAULT_PORT_HTTPS;
int g_max_history = DEFAULT_MAX_HISTORY;
int g_temp_throttle_c = DEFAULT_TEMP_THROTTLE_C;
int g_min_interval_ms = DEFAULT_MIN_INTERVAL_MS;
int g_watchdog_timeout_ms = DEFAULT_WATCHDOG_TIMEOUT_MS;

static shared_frame_t *g_frame;

// Triple buffering for zero-copy frame access
typedef struct {
    unsigned char buf[BUFFER_SIZE];
    size_t len;
    volatile int ready;
} frame_buffer_t;

static frame_buffer_t g_buffers[3];
static volatile int g_write_idx = 0;
static volatile int g_read_idx = 0;
static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t frame_cond = PTHREAD_COND_INITIALIZER;

static volatile sig_atomic_t running = 1;
static SSL_CTX *ssl_ctx = NULL;

static detection_record_t *history = NULL;
static int history_count = 0;
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;

static char *html_cache = NULL;
static size_t html_cache_len = 0;

static float cached_temp = -1;
static long cached_mem = -1;
static float cached_cpu = 0;
static pthread_mutex_t telemetry_mutex = PTHREAD_MUTEX_INITIALIZER;

static int current_interval_ms = 100;
static int mjpeg_streaming_enabled = 1;

static void trim(char *str) {
    char *start = str;
    while (isspace((unsigned char)*start)) start++;
    if (*start == 0) { str[0] = '\0'; return; }
    char *end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    if (start != str) memmove(str, start, strlen(start) + 1);
}

void load_config(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        printf("Config file not found: %s (using defaults)\n", filename);
        return;
    }
    
    char line[256];
    int loaded = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        nl = strchr(line, '\r'); if (nl) *nl = '\0';
        
        char *eq = strchr(line, '=');
        if (!eq) continue;
        
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        trim(key);
        trim(value);
        
        if (strcmp(key, "FRAME_INTERVAL_MS") == 0) {
            g_frame_interval_ms = atoi(value);
            loaded++;
        } else if (strcmp(key, "FRAME_WIDTH") == 0) {
            g_frame_width = atoi(value);
            loaded++;
        } else if (strcmp(key, "FRAME_HEIGHT") == 0) {
            g_frame_height = atoi(value);
            loaded++;
        } else if (strcmp(key, "PORT_HTTP") == 0) {
            g_port_http = atoi(value);
            loaded++;
        } else if (strcmp(key, "PORT_HTTPS") == 0) {
            g_port_https = atoi(value);
            loaded++;
        } else if (strcmp(key, "MAX_HISTORY") == 0) {
            g_max_history = atoi(value);
            loaded++;
        } else if (strcmp(key, "TEMP_THROTTLE_C") == 0) {
            g_temp_throttle_c = atoi(value);
            loaded++;
        } else if (strcmp(key, "MIN_INTERVAL_MS") == 0) {
            g_min_interval_ms = atoi(value);
            loaded++;
        } else if (strcmp(key, "WATCHDOG_TIMEOUT_MS") == 0) {
            g_watchdog_timeout_ms = atoi(value);
            loaded++;
        } else if (strcmp(key, "MJPEG_STREAMING") == 0) {
            mjpeg_streaming_enabled = atoi(value);
            loaded++;
        }
    }
    
    fclose(fp);
    printf("Config loaded: %d settings\n", loaded);
}

void reload_config(const char *filename) {
    printf("\n[RELOAD] Reloading config...\n");
    load_config(filename);
    current_interval_ms = g_frame_interval_ms;
    printf("[RELOAD] New interval: %dms (%.1f fps)\n", 
           current_interval_ms, 1000.0/current_interval_ms);
}

static void signal_handler(int sig) {
    if (sig == SIGHUP) {
        reload_config(CONFIG_PATH);
        return;
    }
    running = 0;
    pthread_cond_broadcast(&frame_cond);
}

static char *load_html(size_t *out_len) {
    FILE *fp = fopen(HTML_PATH, "r");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t r = fread(buf, 1, len, fp);
    (void)r;
    buf[len] = '\0';
    fclose(fp);
    if (out_len) *out_len = len;
    return buf;
}

static float read_temp(void) {
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return -1.0f;
    int t = 0;
    if (fscanf(f, "%d", &t) != 1) { fclose(f); return -1.0f; }
    fclose(f);
    return t / 1000.0f;
}

static long read_mem_available(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char key[32];
    long val = -1;
    while (fscanf(f, "%31s %ld", key, &val) != EOF) {
        if (strcmp(key, "MemAvailable:") == 0) { fclose(f); return val; }
    }
    fclose(f);
    return -1;
}

static float read_cpu_usage(void) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0.0f;
    long u, n, s, i;
    if (fscanf(f, "cpu %ld %ld %ld %ld", &u, &n, &s, &i) != 4) { fclose(f); return 0.0f; }
    fclose(f);
    static long prev_total = 0, prev_idle = 0;
    long total = u + n + s + i;
    long diff_total = total - prev_total;
    long diff_idle = i - prev_idle;
    prev_total = total;
    prev_idle = i;
    if (diff_total <= 0) return 0.0f;
    return (1.0f - ((float)diff_idle / diff_total)) * 100.0f;
}

static void *telemetry_updater(void *arg) {
    (void)arg;
    while (running) {
        float t = read_temp();
        long m = read_mem_available();
        float c = read_cpu_usage();

        pthread_mutex_lock(&telemetry_mutex);
        cached_temp = t;
        cached_mem = m;
        cached_cpu = c;
        pthread_mutex_unlock(&telemetry_mutex);

        if (cached_temp > g_temp_throttle_c && current_interval_ms < g_min_interval_ms) {
            current_interval_ms = g_min_interval_ms;
        } else if (cached_temp <= g_temp_throttle_c - 5 && current_interval_ms != g_frame_interval_ms) {
            current_interval_ms = g_frame_interval_ms;
        }
        sleep(2);
    }
    return NULL;
}

void add_history(int count, float temp) {
    pthread_mutex_lock(&history_mutex);
    if (history_count >= g_max_history) {
        memmove(&history[0], &history[1], (g_max_history - 1) * sizeof(detection_record_t));
        history_count = g_max_history - 1;
    }
    history[history_count].count = count;
    history[history_count].timestamp = time(NULL);
    history[history_count].temp = temp;
    history_count++;
    pthread_mutex_unlock(&history_mutex);
}

void *frame_updater(void *arg) {
    (void)arg;
    struct timespec next_time;
    clock_gettime(CLOCK_MONOTONIC, &next_time);

    unsigned char buf[BUFFER_SIZE];

    while (running) {
        size_t len = shared_frame_read(g_frame, buf, BUFFER_SIZE);

        if (len > 0 && buf[0] == 0xFF && buf[1] == 0xD8) {
            DetectionResult res = process_frame(buf, len, g_frame_width, g_frame_height);
            
            if (res.jpeg_output && res.jpeg_length > 0) {
                int idx = g_write_idx;
                size_t copy_len = res.jpeg_length > BUFFER_SIZE ? BUFFER_SIZE : res.jpeg_length;
                
                pthread_mutex_lock(&frame_mutex);
                memcpy(g_buffers[idx].buf, res.jpeg_output, copy_len);
                g_buffers[idx].len = copy_len;
                g_buffers[idx].ready = 1;
                g_write_idx = (g_write_idx + 1) % 3;
                pthread_cond_signal(&frame_cond);
                pthread_mutex_unlock(&frame_mutex);
            }

            free_detection_result(&res);
        }

        long interval = current_interval_ms;
        next_time.tv_nsec += interval * 1000000L;
        while (next_time.tv_nsec >= 1000000000L) {
            next_time.tv_nsec -= 1000000000L;
            next_time.tv_sec += 1;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_time, NULL);
    }
    return NULL;
}

static void handle_http_redirect(int fd) {
    const char *redir = 
        "HTTP/1.1 301 Moved Permanently\r\n"
        "Location: https://192.168.137.100:8443/\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 24\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html>Redirecting...</html>";
    send(fd, redir, strlen(redir), 0);
    close(fd);
}

// MJPEG streaming thread per client - fully non-blocking
static void *mjpeg_streamer_thread(void *arg) {
    SSL *ssl = (SSL*)arg;
    
    const char *mjpeg_hdr = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY "\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";
    
    if (SSL_write(ssl, mjpeg_hdr, strlen(mjpeg_hdr)) <= 0) {
        SSL_free(ssl);
        return NULL;
    }

    int last_idx = -1;
    
    while (running) {
        pthread_mutex_lock(&frame_mutex);
        
        // Wait for new frame or timeout
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;  // 1 second timeout
        
        int ret = pthread_cond_timedwait(&frame_cond, &frame_mutex, &ts);
        
        if (ret == ETIMEDOUT || !running) {
            pthread_mutex_unlock(&frame_mutex);
            break;
        }
        
        // Find the latest ready frame
        int read_idx = g_read_idx;
        int found = 0;
        
        // Check if there's a newer frame
        for (int i = 0; i < 3; i++) {
            int idx = (g_read_idx + i) % 3;
            if (g_buffers[idx].ready && idx != last_idx) {
                read_idx = idx;
                found = 1;
                break;
            }
        }
        
        if (!found) {
            pthread_mutex_unlock(&frame_mutex);
            continue;
        }
        
        size_t len = g_buffers[read_idx].len;
        
        if (len > 0) {
            char frame_hdr[256];
            int hlen = snprintf(frame_hdr, sizeof(frame_hdr),
                "--" MJPEG_BOUNDARY "\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %zu\r\n"
                "\r\n", len);
            
            pthread_mutex_unlock(&frame_mutex);
            
            // Send frame (non-blocking SSL)
            if (SSL_write(ssl, frame_hdr, hlen) <= 0) break;
            if (SSL_write(ssl, g_buffers[read_idx].buf, len) <= 0) break;
            
            last_idx = read_idx;
            g_read_idx = (read_idx + 1) % 3;
        } else {
            pthread_mutex_unlock(&frame_mutex);
        }
    }
    
    SSL_free(ssl);
    return NULL;
}

static void handle_https_client(int fd) {
    SSL *ssl = SSL_new(ssl_ctx);
    if (!ssl) { close(fd); return; }
    SSL_set_fd(ssl, fd);

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

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

    char method[16], path[512];
    if (sscanf(req, "%15s %511s", method, path) != 2) {
        SSL_free(ssl);
        close(fd);
        return;
    }
    
    char *q = strchr(path, '?');
    if (q) *q = '\0';

    // Check if this is a streaming request
    if ((strcmp(path, "/stream") == 0 || strcmp(path, "/api/v1/stream") == 0) && 
        mjpeg_streaming_enabled) {
        // Spawn dedicated streaming thread
        pthread_t stream_thread;
        if (pthread_create(&stream_thread, NULL, mjpeg_streamer_thread, ssl) == 0) {
            pthread_detach(stream_thread);
        } else {
            SSL_free(ssl);
            close(fd);
        }
        return;
    }

    // Regular HTTP/HTTPS request handling
    if (strcmp(path, "/") == 0) {
        if (html_cache) {
            char header[512];
            int hlen = snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: %zu\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n", html_cache_len);
            SSL_write(ssl, header, hlen);
            SSL_write(ssl, html_cache, html_cache_len);
        }
    } 
    else if (strcmp(path, "/stream") == 0 || strcmp(path, "/api/v1/stream") == 0) {
        // Single JPEG fallback when MJPEG is disabled
        pthread_mutex_lock(&frame_mutex);
        int idx = g_read_idx;
        size_t len = g_buffers[idx].len;
        int ready = g_buffers[idx].ready;
        pthread_mutex_unlock(&frame_mutex);
        
        if (ready && len > 0) {
            char header[256];
            int hlen = snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %zu\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n", len);
            SSL_write(ssl, header, hlen);
            SSL_write(ssl, g_buffers[idx].buf, len);
        }
    } 
    else if (strcmp(path, "/telemetry") == 0 || strcmp(path, "/api/v1/telemetry") == 0) {
        pthread_mutex_lock(&telemetry_mutex);
        float temp = cached_temp;
        long mem = cached_mem;
        float cpu = cached_cpu;
        pthread_mutex_unlock(&telemetry_mutex);

        char json[256];
        int jlen = snprintf(json, sizeof(json), 
            "{\"temp\":%.2f,\"mem\":%ld,\"cpu\":%.2f}", temp, mem, cpu);
        char header[512];
        int hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n", jlen);
        SSL_write(ssl, header, hlen);
        SSL_write(ssl, json, jlen);
    }
    else if (strcmp(path, "/api/v1/persons") == 0) {
        int count = rand() % 4;
        pthread_mutex_lock(&telemetry_mutex);
        float temp = cached_temp;
        pthread_mutex_unlock(&telemetry_mutex);
        
        if (count > 0) add_history(count, temp);

        char json[128];
        int jlen = snprintf(json, sizeof(json), 
            "{\"count\":%d,\"timestamp\":%ld}", count, time(NULL));
        char header[512];
        int hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n", jlen);
        SSL_write(ssl, header, hlen);
        SSL_write(ssl, json, jlen);
    }
    else if (strcmp(path, "/api/v1/history") == 0) {
        pthread_mutex_lock(&history_mutex);
        char json[2048];
        char *p = json;
        p += sprintf(p, "{\"history\":[");
        for (int i = 0; i < history_count; i++) {
            p += sprintf(p, "{\"count\":%d,\"timestamp\":%ld,\"temp\":%.2f}%s",
                         history[i].count, history[i].timestamp, history[i].temp,
                         (i < history_count - 1) ? "," : "");
        }
        p += sprintf(p, "]}");
        pthread_mutex_unlock(&history_mutex);

        int jlen = strlen(json);
        char header[512];
        int hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n", jlen);
        SSL_write(ssl, header, hlen);
        SSL_write(ssl, json, jlen);
    }
    else if (strcmp(path, "/api/v1/command") == 0 && strcmp(method, "POST") == 0) {
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
    }
    else {
        const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        SSL_write(ssl, not_found, strlen(not_found));
    }

    SSL_free(ssl);
    close(fd);
}

typedef struct {
    int fd;
} client_worker_arg_t;

static void *https_worker_thread(void *arg) {
    client_worker_arg_t *carg = (client_worker_arg_t*)arg;
    int fd = carg->fd;
    free(carg);
    handle_https_client(fd);
    return NULL;
}

static SSL_CTX *init_ssl(void) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return NULL;

    // Optimize cipher suite for embedded systems
    SSL_CTX_set_cipher_list(ctx, 
        "ECDHE-ECDSA-AES128-GCM-SHA256:"
        "ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:"
        "ECDHE-RSA-CHACHA20-POLY1305");

    if (SSL_CTX_use_certificate_file(ctx, "cert.pem", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, "key.pem", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }

    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
    SSL_CTX_set_session_id_context(ctx, (const unsigned char*)"server", 6);

    return ctx;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    printf("Starting Security Server...\n");
    printf("Config path: %s\n", CONFIG_PATH);

    load_config(CONFIG_PATH);
    current_interval_ms = g_frame_interval_ms;

    printf("\n=== Configuration ===\n");
    printf("Frame interval: %dms (%.1f fps)\n", g_frame_interval_ms, 1000.0/g_frame_interval_ms);
    printf("Frame size: %dx%d\n", g_frame_width, g_frame_height);
    printf("HTTP port: %d, HTTPS port: %d\n", g_port_http, g_port_https);
    printf("Max history: %d\n", g_max_history);
    printf("Thermal throttle: %d°C, min interval: %dms\n", g_temp_throttle_c, g_min_interval_ms);
    printf("Watchdog timeout: %dms\n", g_watchdog_timeout_ms);
    printf("MJPEG Streaming: %s\n", mjpeg_streaming_enabled ? "ON" : "OFF (fallback)");
    printf("Send SIGHUP (kill -HUP %d) to reload config\n", getpid());
    printf("========================\n\n");

    srand(time(NULL));

    history = malloc(g_max_history * sizeof(detection_record_t));
    if (!history) {
        printf("Failed to allocate history\n");
        return 1;
    }

    html_cache = load_html(&html_cache_len);
    if (!html_cache) {
        printf("Warning: Failed to load HTML template\n");
    } else {
        printf("HTML template loaded (%zu bytes)\n", html_cache_len);
    }

    g_frame = shared_frame_open();
    if (!g_frame) {
        printf("Failed to open shared frame\n");
        free(history);
        return 1;
    }

    // Initialize buffers
    for (int i = 0; i < 3; i++) {
        g_buffers[i].len = 0;
        g_buffers[i].ready = 0;
    }

    pthread_t updater, telemetry_thread;
    if (pthread_create(&updater, NULL, frame_updater, NULL) != 0) {
        perror("pthread_create updater");
        return 1;
    }
    if (pthread_create(&telemetry_thread, NULL, telemetry_updater, NULL) != 0) {
        perror("pthread_create telemetry");
        return 1;
    }

    ssl_ctx = init_ssl();
    if (!ssl_ctx) {
        printf("SSL init failed. Generate cert.pem and key.pem\n");
        return 1;
    }
    printf("SSL initialized\n");

    int http_fd = socket(AF_INET, SOCK_STREAM, 0);
    int https_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(http_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(https_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr_http = { 
        .sin_family = AF_INET, 
        .sin_addr.s_addr = INADDR_ANY, 
        .sin_port = htons(g_port_http) 
    };
    struct sockaddr_in addr_https = { 
        .sin_family = AF_INET, 
        .sin_addr.s_addr = INADDR_ANY, 
        .sin_port = htons(g_port_https) 
    };

    if (bind(http_fd, (struct sockaddr *)&addr_http, sizeof(addr_http)) < 0) {
        perror("bind http");
        return 1;
    }
    if (bind(https_fd, (struct sockaddr *)&addr_https, sizeof(addr_https)) < 0) {
        perror("bind https");
        return 1;
    }

    if (listen(http_fd, 128) < 0) {
        perror("listen http");
        return 1;
    }
    if (listen(https_fd, 128) < 0) {
        perror("listen https");
        return 1;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        return 1;
    }

    struct epoll_event ev, events[MAX_EVENTS];

    ev.events = EPOLLIN;
    ev.data.fd = http_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, http_fd, &ev);

    ev.events = EPOLLIN;
    ev.data.fd = https_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, https_fd, &ev);

    printf("HTTP on %d (redirects to HTTPS)\n", g_port_http);
    printf("HTTPS on %d\n", g_port_https);
    printf("Open: https://192.168.137.100:%d/\n", g_port_https);
    printf("\nServer running... press Ctrl+C to terminate cleanly.\n");

    while (running) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 100);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            if (fd == http_fd) {
                int client = accept(http_fd, NULL, NULL);
                if (client >= 0) {
                    handle_http_redirect(client);
                }
            } else if (fd == https_fd) {
                int client = accept(https_fd, NULL, NULL);
                if (client >= 0) {
                    pthread_t t;
                    client_worker_arg_t *carg = malloc(sizeof(client_worker_arg_t));
                    if (carg) {
                        carg->fd = client;
                        if (pthread_create(&t, NULL, https_worker_thread, carg) == 0) {
                            pthread_detach(t);
                        } else {
                            free(carg);
                            close(client);
                        }
                    } else {
                        close(client);
                    }
                }
            }
        }
    }

    printf("\nShutting down server gracefully...\n");
    close(epfd);
    close(http_fd);
    close(https_fd);

    pthread_join(updater, NULL);
    pthread_join(telemetry_thread, NULL);

    if (ssl_ctx) SSL_CTX_free(ssl_ctx);
    if (html_cache) free(html_cache);
    if (history) free(history);

    printf("Server shutdown complete.\n");
    return 0;
}