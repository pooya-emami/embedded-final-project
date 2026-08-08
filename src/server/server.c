#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>
// #include <openssl/ssl.h>
// #include <openssl/err.h>
#include <time.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>

#include "server.h"
#include "shared_frame.h"
#include "human_detector.hpp"

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
static unsigned char current_frame[BUFFER_SIZE];
static size_t current_len = 0;

static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int running = 1;

// static SSL_CTX *ssl_ctx = NULL;

static detection_record_t *history = NULL;
static int history_count = 0;
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;

static char *html_cache = NULL;

static float cached_temp = -1;
static long cached_mem = -1;
static float cached_cpu = 0;
static pthread_mutex_t telemetry_mutex = PTHREAD_MUTEX_INITIALIZER;

static int current_interval_ms = 100;

static void trim(char *str) {
    char *start = str;
    char *end;
    while (isspace((unsigned char)*start)) start++;
    if (*start == 0) { str[0] = '\0'; return; }
    end = start + strlen(start) - 1;
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
}

static char *load_html(void) {
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
    return buf;
}

static float read_temp(void) {
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return -1;
    int t = 0;
    if (fscanf(f, "%d", &t) != 1) { fclose(f); return -1; }
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
    if (!f) return 0;
    long u, n, s, i;
    if (fscanf(f, "cpu %ld %ld %ld %ld", &u, &n, &s, &i) != 4) { fclose(f); return 0; }
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
        pthread_mutex_lock(&telemetry_mutex);
        cached_temp = read_temp();
        cached_mem = read_mem_available();
        cached_cpu = read_cpu_usage();
        pthread_mutex_unlock(&telemetry_mutex);
        
        if (cached_temp > g_temp_throttle_c && g_frame_interval_ms < g_min_interval_ms) {
            printf("[THERMAL] Temp %.1f°C > %d°C, throttling to %dms\n", 
                   cached_temp, g_temp_throttle_c, g_min_interval_ms);
            current_interval_ms = g_min_interval_ms;
        } else if (cached_temp <= g_temp_throttle_c - 5 && current_interval_ms != g_frame_interval_ms) {
            current_interval_ms = g_frame_interval_ms;
            printf("[THERMAL] Temp %.1f°C, restoring to %dms\n", 
                   cached_temp, current_interval_ms);
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
/*
void *frame_updater(void *arg) {
    (void)arg;

    struct timespec next_time;
    clock_gettime(CLOCK_MONOTONIC, &next_time);

    while (running) {
        unsigned char buf[BUFFER_SIZE];
        size_t len = shared_frame_read(g_frame, buf, BUFFER_SIZE);

        if (len > 0 && buf[0] == 0xFF && buf[1] == 0xD8) {
            // Face detection commented out - pass through raw frame
            // DetectionResult res = process_frame(buf, len, g_frame_width, g_frame_height);
            pthread_mutex_lock(&frame_mutex);
            // size_t copy_len = res.jpeg_length;
            // if (copy_len > BUFFER_SIZE) copy_len = BUFFER_SIZE;
            // memcpy(current_frame, res.jpeg_output, copy_len);
            // current_len = copy_len;
            // Pass through raw frame directly
            size_t copy_len = len;
            if (copy_len > BUFFER_SIZE) copy_len = BUFFER_SIZE;
            memcpy(current_frame, buf, copy_len);
            current_len = copy_len;
            pthread_mutex_unlock(&frame_mutex);
            // free_detection_result(&res);
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
*/
void *frame_updater(void *arg) {
    (void)arg;

    struct timespec next_time;
    clock_gettime(CLOCK_MONOTONIC, &next_time);

static int frame_count = 0;
static struct timespec last_report;
static int timing_initialized = 0;

...

if (len > 0 && len >= 2 &&
    buf[0] == 0xFF && buf[1] == 0xD8) {

    frame_count++;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!timing_initialized) {
        last_report = now;
        timing_initialized = 1;
    }

    double elapsed =
        (now.tv_sec - last_report.tv_sec) +
        (now.tv_nsec - last_report.tv_nsec) / 1e9;

    if (elapsed >= 2.0) {
        printf("[FRAME] %d frames in %.2f sec = %.2f FPS, JPEG=%zu bytes\n",
               frame_count,
               elapsed,
               frame_count / elapsed,
               len);

        frame_count = 0;
        last_report = now;
        fflush(stdout);
    }

    pthread_mutex_lock(&frame_mutex);

    size_t copy_len = len;
    if (copy_len > BUFFER_SIZE)
        copy_len = BUFFER_SIZE;

    memcpy(current_frame, buf, copy_len);
    current_len = copy_len;

    pthread_mutex_unlock(&frame_mutex);
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

static void set_socket_timeout(int fd, int seconds) {
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void send_redirect(int fd) {
    const char *msg =
        "HTTP/1.1 301 Moved Permanently\r\n"
        "Location: http://192.168.137.100:8080/\r\n"  // Changed to HTTP for testing
        "Content-Type: text/html\r\n"
        "Content-Length: 24\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html>Redirecting...</html>";

    send(fd, msg, strlen(msg), 0);
}

// Modified to use plain sockets instead of SSL
static void handle_mjpeg_stream(int fd) {
    const char *header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    if (send(fd, header, strlen(header), 0) <= 0) return;

    struct timespec next_frame;
    clock_gettime(CLOCK_MONOTONIC, &next_frame);

    while (running) {
        pthread_mutex_lock(&frame_mutex);
        size_t len = current_len;
        unsigned char *copy = NULL;
        if (len > 0) {
            copy = malloc(len);
            if (copy) memcpy(copy, current_frame, len);
        }
        pthread_mutex_unlock(&frame_mutex);

        if (copy && len > 0) {
            char part_header[160];
            int hlen = snprintf(part_header, sizeof(part_header),
                "--frame\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %zu\r\n"
                "\r\n", len);

            size_t total = hlen + len + 2;
            unsigned char *sendbuf = malloc(total);
            if (sendbuf) {
                memcpy(sendbuf, part_header, hlen);
                memcpy(sendbuf + hlen, copy, len);
                sendbuf[hlen + len] = '\r';
                sendbuf[hlen + len + 1] = '\n';

                if (send(fd, sendbuf, total, 0) <= 0) {
                    free(sendbuf);
                    free(copy);
                    break;
                }
                free(sendbuf);
            }
        }
        free(copy);

        long interval = current_interval_ms;
        next_frame.tv_nsec += interval * 1000000L;
        while (next_frame.tv_nsec >= 1000000000L) {
            next_frame.tv_nsec -= 1000000000L;
            next_frame.tv_sec += 1;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL);
    }
}

// Modified to use plain sockets instead of SSL
static void *handle_http_thread(void *arg) {
    int fd = *(int*)arg;
    free(arg);

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    char req[4096];
    int n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) {
        close(fd);
        return NULL;
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

            send(fd, header, strlen(header), 0);
            send(fd, html_cache, strlen(html_cache), 0);
        }
        close(fd);
        return NULL;
    }

    if (strcmp(path, "/stream") == 0 || strcmp(path, "/api/v1/stream") == 0) {
        handle_mjpeg_stream(fd);
        close(fd);
        return NULL;
    }

    if (strcmp(path, "/snapshot") == 0 || strcmp(path, "/api/v1/snapshot") == 0) {
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
            send(fd, header, strlen(header), 0);
            send(fd, copy, len, 0);
            free(copy);
        }
        close(fd);
        return NULL;
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

        send(fd, header, strlen(header), 0);
        close(fd);
        return NULL;
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

        send(fd, header, strlen(header), 0);
        close(fd);
        return NULL;
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

        send(fd, header, strlen(header), 0);
        close(fd);
        return NULL;
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
                    send(fd, resp, strlen(resp), 0);
                } else {
                    const char *resp = 
                        "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: 35\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "{\"error\":\"Unknown command\"}";
                    send(fd, resp, strlen(resp), 0);
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
            send(fd, resp, strlen(resp), 0);
        }
        close(fd);
        return NULL;
    }

    send(fd, "HTTP/1.1 404 Not Found\r\n\r\n", 26, 0);
    close(fd);
    return NULL;
}

/* SSL initialization function commented out
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
*/

int main(void) {
    printf("Starting Security Server (TEST MODE - No SSL)...\n");
    printf("Config path: %s\n", CONFIG_PATH);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    // Load config
    load_config(CONFIG_PATH);
    current_interval_ms = g_frame_interval_ms;

    printf("\n=== Configuration ===\n");
    printf("Frame interval: %dms (%.1f fps)\n", g_frame_interval_ms, 1000.0/g_frame_interval_ms);
    printf("Frame size: %dx%d\n", g_frame_width, g_frame_height);
    printf("HTTP port: %d, HTTPS port: %d\n", g_port_http, g_port_https);
    printf("Max history: %d\n", g_max_history);
    printf("Thermal throttle: %d°C, min interval: %dms\n", g_temp_throttle_c, g_min_interval_ms);
    printf("Watchdog timeout: %dms\n", g_watchdog_timeout_ms);
    printf("Send SIGHUP (kill -HUP %d) to reload config\n", getpid());
    printf("========================\n\n");

    srand(time(NULL));

    history = malloc(g_max_history * sizeof(detection_record_t));
    if (!history) {
        printf("Failed to allocate history\n");
        return 1;
    }

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

    /* SSL initialization commented out
    ssl_ctx = init_ssl();
    if (!ssl_ctx) {
        printf("SSL init failed. Generate cert.pem and key.pem\n");
        return 1;
    }

    printf("SSL initialized\n");
    */

    int http_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(http_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(g_port_http)
    };

    if (bind(http_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind http");
        return 1;
    }
    listen(http_fd, 10);
    printf("HTTP server running on port %d (TEST MODE - No SSL redirect)\n", g_port_http);

    /* HTTPS socket creation commented out - using only HTTP for testing
    int https_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(https_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr_https = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(g_port_https)
    };

    if (bind(https_fd, (struct sockaddr *)&addr_https, sizeof(addr_https)) < 0) {
        perror("bind https");
        return 1;
    }
    listen(https_fd, 10);

    printf("HTTPS on %d\n", g_port_https);
    printf("Open: https://192.168.137.100:%d/\n", g_port_https);
    */

    // Use only HTTP port for testing
    printf("Open: http://192.168.137.100:%d/\n", g_port_http);

    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(http_fd, &fds);
        // FD_SET(https_fd, &fds);  // Commented out for testing

        // int max_fd = (https_fd > http_fd) ? https_fd : http_fd;
        int max_fd = http_fd;  // Only HTTP for testing
        
        // Use a timeout so we can check running status periodically
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(max_fd + 1, &fds, NULL, NULL, &tv);
        
        // If select was interrupted or timed out, check running flag
        if (ret < 0) {
            if (errno == EINTR) {
                // Signal interrupted, check running flag
                continue;
            }
            break;
        }
        
        if (ret == 0) {
            // Timeout, check running flag
            continue;
        }

        if (FD_ISSET(http_fd, &fds)) {
            int fd = accept(http_fd, NULL, NULL);
            if (fd >= 0) {
                set_socket_timeout(fd, 30);  // long-lived stream needs a longer timeout
                pthread_t thread;
                int *fd_ptr = malloc(sizeof(int));
                if (fd_ptr) {
                    *fd_ptr = fd;
                    // Use HTTP handler instead of HTTPS
                    pthread_create(&thread, NULL, handle_http_thread, fd_ptr);
                    pthread_detach(thread);
                } else {
                    close(fd);
                }
            }
        }

        /* HTTPS handling commented out
        if (FD_ISSET(https_fd, &fds)) {
            int fd = accept(https_fd, NULL, NULL);
            if (fd >= 0) {
                set_socket_timeout(fd, 30);  // long-lived stream needs a longer timeout
                pthread_t thread;
                int *fd_ptr = malloc(sizeof(int));
                if (fd_ptr) {
                    *fd_ptr = fd;
                    pthread_create(&thread, NULL, handle_https_thread, fd_ptr);
                    pthread_detach(thread);
                } else {
                    close(fd);
                }
            }
        }
        */
    }

    printf("\nShutting down...\n");
    
    // Wait for threads to finish
    pthread_join(updater, NULL);
    pthread_join(telemetry_thread, NULL);

    close(http_fd);
    // close(https_fd);  // Commented out
    // SSL_CTX_free(ssl_ctx);  // Commented out
    free(html_cache);
    free(history);

    printf("Server stopped.\n");
    return 0;
}