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
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <time.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>

#include "server.h"
#include "shared_frame.h"
#include "human_detector.hpp"
#include "email_sender.h"
#include "mqtt_client.h"
#include "sqlite_history.h"

// Global config variables
int g_frame_interval_ms = DEFAULT_FRAME_INTERVAL_MS;
int g_frame_width = DEFAULT_FRAME_WIDTH;
int g_frame_height = DEFAULT_FRAME_HEIGHT;
int g_port_http = DEFAULT_PORT_HTTP;
int g_port_https = DEFAULT_PORT_HTTPS;
int g_max_history = DEFAULT_MAX_HISTORY;
int g_temp_throttle_c = DEFAULT_TEMP_THROTTLE_C;
int g_min_interval_ms = DEFAULT_MIN_INTERVAL_MS;
int g_watchdog_timeout_ms = DEFAULT_WATCHDOG_TIMEOUT_MS;

// SMTP config - accessible to email_sender.c
char g_smtp_server[128] = {0};
char g_smtp_user[128] = {0};
char g_smtp_pass[128] = {0};
char g_smtp_to[128] = {0};

// MQTT config - loaded from config file, NOT hardcoded
static char g_mqtt_host[128] = {0};
static int g_mqtt_port = 1883;
static char g_mqtt_user[128] = {0};
static char g_mqtt_pass[128] = {0};

// Static globals
static shared_frame_t *g_frame = NULL;
static unsigned char current_frame[BUFFER_SIZE];
static size_t current_len = 0;
static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int running = 1;
static SSL_CTX *ssl_ctx = NULL;
static detection_record_t *history = NULL;
static int history_count = 0;
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;
static char *html_cache = NULL;
static float cached_temp = -1;
static long cached_mem = -1;
static float cached_cpu = 0;
static pthread_mutex_t telemetry_mutex = PTHREAD_MUTEX_INITIALIZER;
static int current_interval_ms = 100;
static int guard_enabled = 0;
static int mqtt_initialized = 0;

static int g_person_count = 0;
static time_t g_last_detection_time = 0;
static pthread_mutex_t person_mutex = PTHREAD_MUTEX_INITIALIZER;

// Watchdog globals
static time_t last_frame_time = 0;
static int watchdog_alert_sent = 0;
static int camera_restored_alert_sent = 0;
static unsigned char prev_frame[BUFFER_SIZE] = {0};
static size_t prev_frame_len = 0;
static int first_frame_received = 0;
static pthread_mutex_t watchdog_mutex = PTHREAD_MUTEX_INITIALIZER;

// Email debounce global
static time_t last_email_time = 0;

// Detection state for async processing
static int pending_detection_count = 0;
static float pending_detection_temp = 0;
static unsigned char pending_frame[BUFFER_SIZE] = {0};
static size_t pending_frame_len = 0;
static int pending_is_guard_event = 0;
static int pending_processed = 0;
static pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;

// Guard event tracking
static int active_detection_event = 0;

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
        } else if (strcmp(key, "SMTP_SERVER") == 0) {
            strcpy(g_smtp_server, value);
            loaded++;
        } else if (strcmp(key, "SMTP_USER") == 0) {
            strcpy(g_smtp_user, value);
            loaded++;
        } else if (strcmp(key, "SMTP_PASS") == 0) {
            strcpy(g_smtp_pass, value);
            loaded++;
        } else if (strcmp(key, "SMTP_TO") == 0) {
            strcpy(g_smtp_to, value);
            loaded++;
        } else if (strcmp(key, "MQTT_HOST") == 0) {
            strcpy(g_mqtt_host, value);
            loaded++;
        } else if (strcmp(key, "MQTT_PORT") == 0) {
            g_mqtt_port = atoi(value);
            loaded++;
        } else if (strcmp(key, "MQTT_USER") == 0) {
            strcpy(g_mqtt_user, value);
            loaded++;
        } else if (strcmp(key, "MQTT_PASS") == 0) {
            strcpy(g_mqtt_pass, value);
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
           current_interval_ms, 1000.0 / current_interval_ms);
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
    if (f) {
        int t = 0;
        if (fscanf(f, "%d", &t) == 1) {
            fclose(f);
            return t / 1000.0f;
        }
        fclose(f);
    }
    return -1;
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

void add_history(int count, float temp)
{
    pthread_mutex_lock(&history_mutex);
    if (history_count >= g_max_history) {
        memmove(&history[0], &history[1], (g_max_history - 1) * sizeof(detection_record_t));
        history_count = g_max_history - 1;
    }
    history[history_count].count = count;
    history[history_count].timestamp = time(NULL);
    history[history_count].temp = temp;
    history_count++;
    history_db_add(count, temp);
    pthread_mutex_unlock(&history_mutex);
}

static void send_detection_email(int count, float temp, const unsigned char *buf, size_t len, int immediate)
{
    time_t now = time(NULL);
    
    if (!immediate && (now - last_email_time < 30)) {
        return;
    }
    
    unsigned char *frame_copy = NULL;
    size_t copy_len = 0;
    
    if (buf && len > 0 && buf[0] == 0xFF && buf[1] == 0xD8) {
        frame_copy = malloc(len);
        if (frame_copy) {
            memcpy(frame_copy, buf, len);
            copy_len = len;
        }
    }
    
    if (frame_copy && copy_len > 0) {
        email_send_alert(count, temp, frame_copy, copy_len);
        free(frame_copy);
    } else {
        email_send_alert(count, temp, NULL, 0);
    }
    
    last_email_time = now;
}

static void send_watchdog_alert(const char *status)
{
    time_t now = time(NULL);
    
    // Only send email for tamper events (offline or stuck)
    if (strcmp(status, "offline") == 0 || strcmp(status, "stuck") == 0) {
        email_send_alert_watchdog(cached_temp);
        printf("[WATCHDOG] Camera %s - tamper alert sent\n", status);
    } else {
        // For restored, only log, no email
        printf("[WATCHDOG] Camera %s (no email sent)\n", status);
    }
    
    // Publish to MQTT alarm topic for ALL watchdog events
    if (mqtt_initialized) {
        char topic[128];
        snprintf(topic, sizeof(topic), "alarm/%s/home", STUDENT_ID);
        char payload[256];
        snprintf(payload, sizeof(payload),
            "{\"status\":\"camera_%s\",\"timestamp\":%ld}",
            status, now);
        mqtt_publish_custom(topic, payload);
    }
}

static void process_pending_detection(void)
{
    int count = 0;
    float temp = 0;
    int is_guard_event = 0;
    unsigned char frame[BUFFER_SIZE];
    size_t frame_len = 0;
    int should_process = 0;
    
    pthread_mutex_lock(&pending_mutex);
    if (!pending_processed && pending_detection_count > 0) {
        count = pending_detection_count;
        temp = pending_detection_temp;
        is_guard_event = pending_is_guard_event;
        frame_len = pending_frame_len;
        if (frame_len > 0 && frame_len < BUFFER_SIZE) {
            memcpy(frame, pending_frame, frame_len);
        }
        pending_processed = 1;
        should_process = 1;
    }
    pthread_mutex_unlock(&pending_mutex);
    
    if (!should_process) return;
    
    add_history(count, temp);
    
    if (mqtt_initialized) {
        mqtt_publish_persons(count, temp);
    }
    
    if (is_guard_event) {
        // MQTT alarm - ALWAYS send on every detection
        if (mqtt_initialized) {
            mqtt_publish_alarm(count, temp);
        }
        
        // Email - ONLY on NEW events (person just appeared)
        if (frame_len > 0) {
            email_send_alert_guard(count, temp, frame, frame_len);
        } else {
            email_send_alert_guard(count, temp, NULL, 0);
        }
        printf("[PROCESS] GUARD email sent: %d person(s)\n", count);
        
    } else {
        // Normal mode: send email with 30-second debounce
        send_detection_email(count, temp, frame, frame_len, 0);
    }
}

static void *telemetry_updater(void *arg) {
    (void)arg;
    while (running) {
        pthread_mutex_lock(&telemetry_mutex);
        cached_temp = read_temp();
        cached_mem = read_mem_available();
        cached_cpu = read_cpu_usage();
        pthread_mutex_unlock(&telemetry_mutex);
        
        process_pending_detection();
        
        if (mqtt_initialized) {
            mqtt_publish_telemetry(cached_temp, cached_mem, cached_cpu);
        }
        
        if (cached_temp > g_temp_throttle_c && current_interval_ms > g_min_interval_ms) {
            printf("[THERMAL] Temp %.1f C > %d C, throttling to %dms\n", 
                   cached_temp, g_temp_throttle_c, g_min_interval_ms);
            current_interval_ms = g_min_interval_ms;
        } else if (cached_temp <= g_temp_throttle_c - 5 && current_interval_ms != g_frame_interval_ms) {
            current_interval_ms = g_frame_interval_ms;
            printf("[THERMAL] Temp %.1f C, restoring to %dms\n", 
                   cached_temp, current_interval_ms);
        }
        
        sleep(2);
    }
    return NULL;
}

void *frame_updater(void *arg) {
    (void)arg;

    struct timespec next_time;
    clock_gettime(CLOCK_MONOTONIC, &next_time);

    int no_detection_frame_count = 0;
    int last_person_count = 0;

    while (running) {
        unsigned char buf[BUFFER_SIZE];
        size_t len = shared_frame_read(g_frame, buf, BUFFER_SIZE);

        // WATCHDOG: Check if frame changed
        if (len > 0 && buf[0] == 0xFF && buf[1] == 0xD8) {
            pthread_mutex_lock(&watchdog_mutex);
            
            if (!first_frame_received) {
                memcpy(prev_frame, buf, len);
                prev_frame_len = len;
                last_frame_time = time(NULL);
                first_frame_received = 1;
                printf("[WATCHDOG] First frame received (%zu bytes)\n", len);
            } else if (len != prev_frame_len || memcmp(buf, prev_frame, len) != 0) {
                memcpy(prev_frame, buf, len);
                prev_frame_len = len;
                last_frame_time = time(NULL);
                
                if (watchdog_alert_sent && !camera_restored_alert_sent) {
                    camera_restored_alert_sent = 1;
                    watchdog_alert_sent = 0;
                    send_watchdog_alert("restored");
                    printf("[WATCHDOG] Camera restored!\n");
                }
            }
            
            pthread_mutex_unlock(&watchdog_mutex);
        }

        if (len > 0 && buf[0] == 0xFF && buf[1] == 0xD8) {
            DetectionResult res = process_frame(buf, len, g_frame_width, g_frame_height);
            
            pthread_mutex_lock(&person_mutex);
            g_person_count = res.person_count;
            g_last_detection_time = time(NULL);
            pthread_mutex_unlock(&person_mutex);
            
            pthread_mutex_lock(&frame_mutex);
            size_t copy_len = res.jpeg_length;
            if (copy_len > BUFFER_SIZE) copy_len = BUFFER_SIZE;
            if (res.jpeg_output) {
                memcpy(current_frame, res.jpeg_output, copy_len);
                current_len = copy_len;
            }
            pthread_mutex_unlock(&frame_mutex);
            
            pthread_mutex_lock(&telemetry_mutex);
            float temp = cached_temp;
            pthread_mutex_unlock(&telemetry_mutex);
            
            if (res.person_count > 0) {
                no_detection_frame_count = 0;
                
                // Check if this is a NEW event (person just appeared)
                int is_new_event = 0;
                if (active_detection_event == 0) {
                    is_new_event = 1;
                    active_detection_event = 1;
                    printf("[GUARD] New detection event started\n");
                }
                last_person_count = res.person_count;
                
                pthread_mutex_lock(&pending_mutex);
                pending_detection_count = res.person_count;
                pending_detection_temp = temp;
                pending_is_guard_event = guard_enabled && is_new_event;
                pending_processed = 0;
                
                if (len > 0 && len < BUFFER_SIZE) {
                    memcpy(pending_frame, buf, len);
                    pending_frame_len = len;
                }
                pthread_mutex_unlock(&pending_mutex); 
                
            } else {
                no_detection_frame_count++;
                
                // Person left - reset event after 100+ frames (~3 seconds)
                if (no_detection_frame_count > 100 && active_detection_event) {
                    active_detection_event = 0;
                    printf("[GUARD] Person left, resetting detection event\n");
                }
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

// ============================================================
// WATCHDOG MONITOR - Checks for camera tampering
// ============================================================
void *watchdog_monitor(void *arg) {
    (void)arg;
    
    while (running) {
        pthread_mutex_lock(&watchdog_mutex);
        
        int timeout_seconds = g_watchdog_timeout_ms / 1000;
        if (timeout_seconds <= 0) timeout_seconds = 30;
        
        time_t now = time(NULL);
        
        if (now - last_frame_time > timeout_seconds) {
            if (!watchdog_alert_sent) {
                watchdog_alert_sent = 1;
                camera_restored_alert_sent = 0;
                send_watchdog_alert("offline");
                printf("[WATCHDOG] Camera offline/stuck! No change for %d seconds\n", timeout_seconds);
            }
        }
        
        pthread_mutex_unlock(&watchdog_mutex);
        sleep(1);
    }
    return NULL;
}

static void set_socket_timeout(int fd, int seconds) {
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void send_redirect(int fd, const char *host_header) {
    char location[256];
    if (host_header && strlen(host_header) > 0) {
        snprintf(location, sizeof(location), 
                 "https://%s:%d/", host_header, g_port_https);
    } else {
        snprintf(location, sizeof(location), 
                 "https://localhost:%d/", g_port_https);
    }
    
    char msg[512];
    snprintf(msg, sizeof(msg),
        "HTTP/1.1 301 Moved Permanently\r\n"
        "Location: %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 24\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html>Redirecting...</html>",
        location);
    send(fd, msg, strlen(msg), 0);
}

static void handle_mjpeg_stream(SSL *ssl) {
    const char *header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    if (SSL_write(ssl, header, strlen(header)) <= 0) return;

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

                if (SSL_write(ssl, sendbuf, total) <= 0) {
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

static void *handle_https_thread(void *arg) {
    int fd = *(int*)arg;
    free(arg);

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    SSL *ssl = SSL_new(ssl_ctx);
    SSL_set_fd(ssl, fd);

    if (SSL_accept(ssl) <= 0) {
        SSL_free(ssl);
        close(fd);
        return NULL;
    }

    char req[4096];
    int n = SSL_read(ssl, req, sizeof(req) - 1);
    if (n <= 0) {
        SSL_free(ssl);
        close(fd);
        return NULL;
    }

    req[n] = '\0';

    char method[16], path[256];
    sscanf(req, "%15s %255s", method, path);

    char *q = strchr(path, '?');
    if (q) *q = '\0';

    // GET /
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
        return NULL;
    }

    // GET /stream or /api/v1/stream
    if (strcmp(path, "/stream") == 0 || strcmp(path, "/api/v1/stream") == 0) {
        handle_mjpeg_stream(ssl);
        SSL_free(ssl);
        close(fd);
        return NULL;
    }

    // GET /raw_stream - Raw camera feed (no detection overlay)
    if (strcmp(path, "/raw_stream") == 0) {
        unsigned char raw_buf[BUFFER_SIZE];
        size_t raw_len = shared_frame_read(g_frame, raw_buf, BUFFER_SIZE);
        
        if (raw_len > 0 && raw_buf[0] == 0xFF && raw_buf[1] == 0xD8) {
            char header[256];
            snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %zu\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n",
                raw_len);
            SSL_write(ssl, header, strlen(header));
            SSL_write(ssl, raw_buf, raw_len);
        } else {
            const char *resp = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
            SSL_write(ssl, resp, strlen(resp));
        }
        SSL_free(ssl);
        close(fd);
        return NULL;
    }

    // GET /snapshot or /api/v1/snapshot
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
            SSL_write(ssl, header, strlen(header));
            SSL_write(ssl, copy, len);
            free(copy);
        }
        SSL_free(ssl);
        close(fd);
        return NULL;
    }

    // GET /telemetry or /api/v1/telemetry
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
        return NULL;
    }

    // GET /api/v1/persons
    if (strcmp(path, "/api/v1/persons") == 0) {
        int count = 0;
        pthread_mutex_lock(&person_mutex);
        count = g_person_count;
        pthread_mutex_unlock(&person_mutex);

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
        return NULL;
    }

    // GET /api/v1/history
    if (strcmp(path, "/api/v1/history") == 0) {
        history_record_t *records = malloc(sizeof(history_record_t) * g_max_history);
        int n = history_db_get_last(records, g_max_history);

        char json[2048];
        char *p = json;
        p += sprintf(p, "{\"history\":[");

        for (int i = 0; i < n; i++) {
            p += sprintf(p,
                "{\"count\":%d,\"temp\":%.2f,\"timestamp\":%ld}",
                records[i].count,
                records[i].temp,
                records[i].timestamp
            );
            if (i < n - 1) p += sprintf(p, ",");
        }

        p += sprintf(p, "]}");
        free(records);

        char header[4096];
        snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n%s",
            strlen(json), json);

        SSL_write(ssl, header, strlen(header));
        SSL_free(ssl);
        close(fd);
        return NULL;
    }

    // GET /api/v1/history_total
    if (strcmp(path, "/api/v1/history_total") == 0) {
        long total = history_db_total();

        char json[128];
        snprintf(json, sizeof(json),
                 "{\"total\":%ld}", total);

        char header[256];
        snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n%s",
            strlen(json), json);

        SSL_write(ssl, header, strlen(header));
        SSL_free(ssl);
        close(fd);
        return NULL;
    }

    // POST /api/v1/guard
    if (strcmp(path, "/api/v1/guard") == 0) {
        if (strcmp(method, "POST") == 0) {
            char *body = strstr(req, "\r\n\r\n");
            if (body) {
                body += 4;
                if (strstr(body, "\"enabled\":true")) {
                    guard_enabled = 1;
                } else if (strstr(body, "\"enabled\":false")) {
                    guard_enabled = 0;
                }
            }
            char resp[256];
            snprintf(resp, sizeof(resp),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Connection: close\r\n"
                "\r\n"
                "{\"guard_enabled\":%s}",
                guard_enabled ? "true" : "false");
            SSL_write(ssl, resp, strlen(resp));
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
        return NULL;
    }

    // POST /api/v1/command
    if (strcmp(path, "/api/v1/command") == 0) {
        if (strcmp(method, "POST") == 0) {
            char *body = strstr(req, "\r\n\r\n");
            char json_resp[256];
            
            if (body) {
                body += 4;
                char *cmd_start = strstr(body, "\"cmd\":\"");
                if (cmd_start) {
                    cmd_start += 7;
                    char *cmd_end = strchr(cmd_start, '"');
                    if (cmd_end) {
                        *cmd_end = '\0';
                        char command[64];
                        strncpy(command, cmd_start, sizeof(command) - 1);
                        command[sizeof(command) - 1] = '\0';
                        
                        if (strcmp(command, "reboot") == 0) {
                            printf("[CMD] Executing reboot\n");
                            snprintf(json_resp, sizeof(json_resp),
                                     "{\"status\":\"success\",\"cmd\":\"reboot\"}");
                            system("shutdown -r now &");
                        } else if (strcmp(command, "shutdown") == 0) {
                            printf("[CMD] Executing shutdown\n");
                            snprintf(json_resp, sizeof(json_resp),
                                     "{\"status\":\"success\",\"cmd\":\"shutdown\"}");
                            system("shutdown -h now &");
                        } else if (strcmp(command, "restart_detection") == 0) {
                            printf("[CMD] Restarting detection\n");
                            snprintf(json_resp, sizeof(json_resp),
                                     "{\"status\":\"success\",\"cmd\":\"restart_detection\"}");
                            system("systemctl restart detection.service 2>/dev/null &");
                        } else {
                            snprintf(json_resp, sizeof(json_resp),
                                     "{\"status\":\"success\",\"cmd\":\"%s\"}", command);
                            char sys_cmd[128];
                            snprintf(sys_cmd, sizeof(sys_cmd), "%s &", command);
                            system(sys_cmd);
                        }
                        
                        char header[512];
                        snprintf(header, sizeof(header),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: application/json\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n"
                            "\r\n%s",
                            strlen(json_resp), json_resp);
                        SSL_write(ssl, header, strlen(header));
                        SSL_free(ssl);
                        close(fd);
                        return NULL;
                    }
                }
            }
            
            const char *resp = 
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 42\r\n"
                "Connection: close\r\n"
                "\r\n"
                "{\"error\":\"Invalid or missing command\"}";
            SSL_write(ssl, resp, strlen(resp));
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
        return NULL;
    }

    // 404 Not Found
    SSL_write(ssl, "HTTP/1.1 404 Not Found\r\n\r\n", 26);
    SSL_free(ssl);
    close(fd);
    return NULL;
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
    printf("Config path: %s\n", CONFIG_PATH);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    load_config(CONFIG_PATH);
    current_interval_ms = g_frame_interval_ms;

    printf("\n=== Configuration ===\n");
    printf("Frame interval: %dms (%.1f fps)\n", g_frame_interval_ms, 1000.0 / g_frame_interval_ms);
    printf("Frame size: %dx%d\n", g_frame_width, g_frame_height);
    printf("HTTP port: %d, HTTPS port: %d\n", g_port_http, g_port_https);
    printf("Max history: %d\n", g_max_history);
    printf("Thermal throttle: %d C, min interval: %dms\n", g_temp_throttle_c, g_min_interval_ms);
    printf("Watchdog timeout: %dms\n", g_watchdog_timeout_ms);
    printf("Send SIGHUP (kill -HUP %d) to reload config\n", getpid());
    printf("========================\n\n");

    srand(time(NULL));

    if (history_db_init("/home/pooya/embproj/proj/security_history.db") != 0) {
        printf("Failed to initialize SQLite history DB\n");
    }

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

    last_frame_time = time(NULL);

    pthread_t updater, telemetry_thread, watchdog_thread;
    pthread_create(&updater, NULL, frame_updater, NULL);
    pthread_create(&telemetry_thread, NULL, telemetry_updater, NULL);
    pthread_create(&watchdog_thread, NULL, watchdog_monitor, NULL);

    if (strlen(g_mqtt_host) > 0) {
        mqtt_init(g_mqtt_host, g_mqtt_port, g_mqtt_user, g_mqtt_pass);
        mqtt_initialized = 1;
        printf("[MQTT] Initialized with host: %s\n", g_mqtt_host);
    } else {
        printf("[MQTT] No host configured, skipping\n");
    }

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
        .sin_port = htons(g_port_http)
    };

    if (bind(http_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind http");
        return 1;
    }
    listen(http_fd, 10);
    printf("HTTP on %d (redirects to HTTPS)\n", g_port_http);

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
    printf("Open: https://localhost:%d/\n", g_port_https);

    if (mqtt_initialized) {
        pthread_mutex_lock(&telemetry_mutex);
        mqtt_publish_telemetry(cached_temp, cached_mem, cached_cpu);
        pthread_mutex_unlock(&telemetry_mutex);
    }

    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(http_fd, &fds);
        FD_SET(https_fd, &fds);

        int max_fd = (https_fd > http_fd) ? https_fd : http_fd;
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(max_fd + 1, &fds, NULL, NULL, &tv);
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        
        if (ret == 0) continue;

        if (FD_ISSET(http_fd, &fds)) {
            int fd = accept(http_fd, NULL, NULL);
            if (fd >= 0) {
                set_socket_timeout(fd, 5);
                send_redirect(fd, NULL);
                close(fd);
            }
        }

        if (FD_ISSET(https_fd, &fds)) {
            int fd = accept(https_fd, NULL, NULL);
            if (fd >= 0) {
                set_socket_timeout(fd, 30);
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
    }

    printf("\nShutting down...\n");
    pthread_join(updater, NULL);
    pthread_join(telemetry_thread, NULL);
    pthread_join(watchdog_thread, NULL);

    close(http_fd);
    close(https_fd);
    SSL_CTX_free(ssl_ctx);
    free(html_cache);
    free(history);
    history_db_close();

    printf("Server stopped.\n");
    return 0;
}