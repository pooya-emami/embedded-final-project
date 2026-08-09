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
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>

#include "server.h"
#include "shared_frame.h"
#include "shared_frame_processed.h"
#include "email_sender.h"
#include "mqtt_client.h"
#include "sqlite_history.h"

int g_frame_interval_ms = DEFAULT_FRAME_INTERVAL_MS;
int g_frame_width = DEFAULT_FRAME_WIDTH;
int g_frame_height = DEFAULT_FRAME_HEIGHT;
int g_port_http = DEFAULT_PORT_HTTP;
int g_port_https = DEFAULT_PORT_HTTPS;
int g_max_history = DEFAULT_MAX_HISTORY;
int g_temp_throttle_c = DEFAULT_TEMP_THROTTLE_C;
int g_min_interval_ms = DEFAULT_MIN_INTERVAL_MS;
int g_watchdog_timeout_ms = DEFAULT_WATCHDOG_TIMEOUT_MS;

char g_smtp_server[128] = {0};
char g_smtp_user[128] = {0};
char g_smtp_pass[128] = {0};
char g_smtp_to[128] = {0};

static char g_mqtt_host[128] = {0};
static int g_mqtt_port = 1883;
static char g_mqtt_user[128] = {0};
static char g_mqtt_pass[128] = {0};

static char g_db_path[256] = {0};

static shared_frame_t *g_frame = NULL;
static processed_frame_t *g_processed = NULL;
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

static time_t last_frame_time = 0;
static int watchdog_alert_sent = 0;
static int camera_restored_alert_sent = 0;
static pthread_mutex_t watchdog_mutex = PTHREAD_MUTEX_INITIALIZER;

static unsigned char prev_frame[SHM_FRAME_BUF_SIZE] = {0};
static size_t prev_frame_len = 0;
static int first_frame_received = 0;
static int frame_stuck_count = 0;

static time_t last_email_time = 0;
static int active_detection_event = 0;
static int stream_mode = 0;

static int history_pending = 0;
static int history_count_val = 0;
static float history_temp = 0;
static pthread_mutex_t history_mutex_signal = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t history_cond = PTHREAD_COND_INITIALIZER;
static int history_thread_running = 1;
static pthread_t history_thread;

static int email_pending = 0;
static int email_count = 0;
static float email_temp = 0;
static unsigned char email_frame[BUFFER_SIZE];
static size_t email_frame_len = 0;
static int email_immediate = 0;
static int email_has_frame = 0;
static pthread_mutex_t email_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t email_cond = PTHREAD_COND_INITIALIZER;
static int email_thread_running = 1;
static pthread_t email_thread;

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
        } else if (strcmp(key, "HISTORY_DB_PATH") == 0) {
            strcpy(g_db_path, value);
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

    f = fopen("/mnt/d/Users/ASUS/Documents/Virtual Machines/shared/cpu.txt", "r");
    if (f) {
        float t = -1;
        if (fscanf(f, "%f", &t) == 1) {
            fclose(f);
            return t;
        }
        fclose(f);
    }

    f = fopen("/mnt/hgfs/shared/cpu.txt", "r");
    if (f) {
        float t = -1;
        if (fscanf(f, "%f", &t) == 1) {
            fclose(f);
            return t;
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

static void add_history_internal(int count, float temp)
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

static void *telemetry_updater(void *arg) {
    (void)arg;
    
    static int original_width = 0;
    static int original_height = 0;
    static int throttled = 0;
    static int throttle_email_sent = 0;
    
    if (original_width == 0) {
        original_width = g_frame_width;
        original_height = g_frame_height;
    }
    
    while (running) {
        pthread_mutex_lock(&telemetry_mutex);
        cached_temp = read_temp();
        cached_mem = read_mem_available();
        cached_cpu = read_cpu_usage();
        pthread_mutex_unlock(&telemetry_mutex);
        
        if (cached_temp > g_temp_throttle_c) {
            if (!throttled) {
                current_interval_ms = (int)(g_frame_interval_ms * 1.5);
                if (current_interval_ms < g_min_interval_ms) {
                    current_interval_ms = g_min_interval_ms;
                }
                
                g_frame_width = original_width / 2;
                g_frame_height = original_height / 2;
                throttled = 1;
                throttle_email_sent = 0;
                
                printf("[THERMAL] Temp %.1f C > %d C, throttling:\n", 
                       cached_temp, g_temp_throttle_c);
                printf("[THERMAL]   FPS: %.1f -> %.1f\n",
                       1000.0 / g_frame_interval_ms, 1000.0 / current_interval_ms);
                printf("[THERMAL]   Resolution: %dx%d -> %dx%d\n",
                       original_width, original_height,
                       g_frame_width, g_frame_height);
            }
            
            if (!throttle_email_sent) {
                email_send_alert_thermal(cached_temp);
                throttle_email_sent = 1;
            }
            
        } else if (cached_temp <= g_temp_throttle_c - 5 && throttled) {
            current_interval_ms = g_frame_interval_ms;
            g_frame_width = original_width;
            g_frame_height = original_height;
            throttled = 0;
            throttle_email_sent = 0;
            
            printf("[THERMAL] Temp %.1f C <= %d C, restored:\n", 
                   cached_temp, g_temp_throttle_c - 5);
            printf("[THERMAL]   FPS: %.1f -> %.1f\n",
                   1000.0 / current_interval_ms, 1000.0 / g_frame_interval_ms);
            printf("[THERMAL]   Resolution: %dx%d -> %dx%d\n",
                   original_width / 2, original_height / 2,
                   original_width, original_height);
        }
        
        if (mqtt_initialized) {
            mqtt_publish_telemetry(cached_temp, cached_mem, cached_cpu);
        }
        
        sleep(2);
    }
    return NULL;
}

static void send_watchdog_alert(const char *status)
{
    time_t now = time(NULL);
    
    if (strcmp(status, "offline") == 0 || strcmp(status, "stuck") == 0) {
        email_send_alert_watchdog(cached_temp);
        printf("[WATCHDOG] Camera %s - tamper alert sent\n", status);
    } else {
        printf("[WATCHDOG] Camera %s (no email sent)\n", status);
    }
    
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

static void *history_thread_func(void *arg) {
    (void)arg;
    
    while (history_thread_running) {
        int count = 0;
        float temp = 0;
        
        pthread_mutex_lock(&history_mutex_signal);
        while (!history_pending && history_thread_running) {
            pthread_cond_wait(&history_cond, &history_mutex_signal);
        }
        
        if (!history_thread_running) {
            pthread_mutex_unlock(&history_mutex_signal);
            break;
        }
        
        if (history_pending) {
            count = history_count_val;
            temp = history_temp;
            history_pending = 0;
        }
        pthread_mutex_unlock(&history_mutex_signal);
        
        add_history_internal(count, temp);
    }
    return NULL;
}

static void signal_history(int count, float temp)
{
    pthread_mutex_lock(&history_mutex_signal);
    history_count_val = count;
    history_temp = temp;
    history_pending = 1;
    pthread_cond_signal(&history_cond);
    pthread_mutex_unlock(&history_mutex_signal);
}

static void signal_email(int count, float temp, const unsigned char *buf, size_t len, int immediate)
{
    pthread_mutex_lock(&email_mutex);
    
    email_count = count;
    email_temp = temp;
    email_immediate = immediate;
    email_pending = 1;
    
    if (buf && len > 0 && len < BUFFER_SIZE) {
        memcpy(email_frame, buf, len);
        email_frame_len = len;
        email_has_frame = 1;
    } else {
        email_frame_len = 0;
        email_has_frame = 0;
    }
    
    pthread_cond_signal(&email_cond);
    pthread_mutex_unlock(&email_mutex);
}

static void *email_thread_func(void *arg) {
    (void)arg;
    
    while (email_thread_running) {
        int count = 0;
        float temp = 0;
        int immediate = 0;
        unsigned char frame[BUFFER_SIZE];
        size_t frame_len = 0;
        int has_frame = 0;
        
        pthread_mutex_lock(&email_mutex);
        while (!email_pending && email_thread_running) {
            pthread_cond_wait(&email_cond, &email_mutex);
        }
        
        if (!email_thread_running) {
            pthread_mutex_unlock(&email_mutex);
            break;
        }
        
        if (email_pending) {
            count = email_count;
            temp = email_temp;
            immediate = email_immediate;
            has_frame = email_has_frame;
            frame_len = email_frame_len;
            if (has_frame && frame_len > 0 && frame_len < BUFFER_SIZE) {
                memcpy(frame, email_frame, frame_len);
            }
            email_pending = 0;
        }
        pthread_mutex_unlock(&email_mutex);
        
        if (guard_enabled) {
            if (immediate) {
                unsigned char *frame_copy = NULL;
                if (has_frame && frame_len > 0) {
                    frame_copy = malloc(frame_len);
                    if (frame_copy) {
                        memcpy(frame_copy, frame, frame_len);
                    }
                }
                
                if (frame_copy && frame_len > 0) {
                    email_send_alert_guard(count, temp, frame_copy, frame_len);
                } else {
                    email_send_alert_guard(count, temp, NULL, 0);
                }
                free(frame_copy);
                printf("[EMAIL] Guard email sent: %d person(s)\n", count);
            }
        } else {
            unsigned char *frame_copy = NULL;
            if (has_frame && frame_len > 0) {
                frame_copy = malloc(frame_len);
                if (frame_copy) {
                    memcpy(frame_copy, frame, frame_len);
                }
            }
            
            time_t now = time(NULL);
            if (now - last_email_time >= 30) {
                if (frame_copy && frame_len > 0) {
                    email_send_alert(count, temp, frame_copy, frame_len);
                } else {
                    email_send_alert(count, temp, NULL, 0);
                }
                free(frame_copy);
                last_email_time = now;
                printf("[EMAIL] Normal email sent: %d person(s)\n", count);
            } else {
                free(frame_copy);
            }
        }
    }
    return NULL;
}

static void run_command(const char *cmd)
{
    int ret = system(cmd);
    (void)ret;
}

static unsigned char blank_jpeg[] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x01, 0x00, 0x48,
    0x00, 0x48, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x10, 0x0B, 0x0C, 0x0E, 0x0C, 0x0A, 0x10,
    0x0E, 0x0D, 0x0E, 0x12, 0x11, 0x10, 0x13, 0x18, 0x28, 0x1A, 0x18, 0x16, 0x16, 0x18, 0x31, 0x23,
    0x25, 0x1D, 0x28, 0x3A, 0x33, 0x3D, 0x3C, 0x39, 0x33, 0x38, 0x37, 0x40, 0x48, 0x5C, 0x4E, 0x40,
    0x44, 0x57, 0x45, 0x37, 0x38, 0x50, 0x6D, 0x51, 0x57, 0x5F, 0x62, 0x67, 0x68, 0x67, 0x3E, 0x4D,
    0x71, 0x79, 0x70, 0x64, 0x78, 0x5C, 0x65, 0x67, 0x63, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x80,
    0x00, 0x80, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03,
    0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05,
    0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1,
    0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A,
    0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5,
    0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3,
    0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9,
    0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xDA, 0x00, 0x0C, 0x03,
    0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00, 0xF9, 0xC2, 0x8A, 0x28, 0xA0, 0x0F, 0xFF,
    0xD9
};
static size_t blank_jpeg_len = 0;

static void handle_mjpeg_stream(SSL *ssl) {
    const char *header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    if (SSL_write(ssl, header, strlen(header)) <= 0) return;

    if (blank_jpeg_len == 0) {
        blank_jpeg_len = sizeof(blank_jpeg);
    }

    struct timespec next_frame;
    clock_gettime(CLOCK_MONOTONIC, &next_frame);

    while (running) {
        if (stream_mode == 0) {
            char part_header[160];
            int hlen = snprintf(part_header, sizeof(part_header),
                "--frame\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %zu\r\n"
                "\r\n", blank_jpeg_len);

            size_t total = hlen + blank_jpeg_len + 2;
            unsigned char *sendbuf = malloc(total);
            if (sendbuf) {
                memcpy(sendbuf, part_header, hlen);
                memcpy(sendbuf + hlen, blank_jpeg, blank_jpeg_len);
                sendbuf[hlen + blank_jpeg_len] = '\r';
                sendbuf[hlen + blank_jpeg_len + 1] = '\n';

                if (SSL_write(ssl, sendbuf, total) <= 0) {
                    free(sendbuf);
                    break;
                }
                free(sendbuf);
            }
            
        } else if (stream_mode == 1) {
            unsigned char raw_buf[SHM_FRAME_BUF_SIZE];
            size_t raw_len = shared_frame_read(g_frame, raw_buf, SHM_FRAME_BUF_SIZE);
            
            if (raw_len > 0 && raw_buf[0] == 0xFF && raw_buf[1] == 0xD8) {
                char part_header[160];
                int hlen = snprintf(part_header, sizeof(part_header),
                    "--frame\r\n"
                    "Content-Type: image/jpeg\r\n"
                    "Content-Length: %zu\r\n"
                    "\r\n", raw_len);

                size_t total = hlen + raw_len + 2;
                unsigned char *sendbuf = malloc(total);
                if (sendbuf) {
                    memcpy(sendbuf, part_header, hlen);
                    memcpy(sendbuf + hlen, raw_buf, raw_len);
                    sendbuf[hlen + raw_len] = '\r';
                    sendbuf[hlen + raw_len + 1] = '\n';

                    if (SSL_write(ssl, sendbuf, total) <= 0) {
                        free(sendbuf);
                        break;
                    }
                    free(sendbuf);
                }
            }
            
        } else {
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
                free(copy);
            }
        }

        long interval = current_interval_ms;
        next_frame.tv_nsec += interval * 1000000L;
        while (next_frame.tv_nsec >= 1000000000L) {
            next_frame.tv_nsec -= 1000000000L;
            next_frame.tv_sec += 1;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL);
    }
}

void *frame_updater(void *arg) {
    (void)arg;

    struct timespec next_time;
    clock_gettime(CLOCK_MONOTONIC, &next_time);

    int no_detection_frame_count = 0;
    int last_person_count = 0;
    int count = 0;
    float temp = 0;

    while (running) {
        if (stream_mode == 2 && g_processed) {
            unsigned char proc_buf[BUFFER_SIZE];
            size_t proc_len = processed_frame_read(g_processed, proc_buf, BUFFER_SIZE, &count, &temp);
            
            if (proc_len > 0) {
                pthread_mutex_lock(&frame_mutex);
                size_t copy_len = proc_len;
                if (copy_len > BUFFER_SIZE) copy_len = BUFFER_SIZE;
                memcpy(current_frame, proc_buf, copy_len);
                current_len = copy_len;
                pthread_mutex_unlock(&frame_mutex);
                
                pthread_mutex_lock(&person_mutex);
                g_person_count = count;
                g_last_detection_time = time(NULL);
                pthread_mutex_unlock(&person_mutex);
                
                if (count > 0) {
                    no_detection_frame_count = 0;
                    
                    int is_new_event = 0;
                    if (no_detection_frame_count > 100 || 
                        count != last_person_count ||
                        active_detection_event == 0) {
                        is_new_event = 1;
                        active_detection_event = 1;
                        if (guard_enabled) {
                            printf("[GUARD] New detection event started (count: %d)\n", count);
                        }
                    }
                    last_person_count = count;
                    
                    if (mqtt_initialized) {
                        mqtt_publish_persons(count, temp);
                        if (guard_enabled) {
                            mqtt_publish_alarm(count, temp);
                        }
                    }
                    
                    signal_history(count, temp);
                    
                    int immediate = guard_enabled && is_new_event;
                    signal_email(count, temp, current_frame, current_len, immediate);
                } else {
                    no_detection_frame_count++;
                    
                    if (no_detection_frame_count > 100 && active_detection_event) {
                        active_detection_event = 0;
                        if (guard_enabled) {
                            printf("[GUARD] Person left, resetting detection event\n");
                        }
                    }
                }
            }
        } else {
            unsigned char raw_buf[SHM_FRAME_BUF_SIZE];
            size_t raw_len = shared_frame_read(g_frame, raw_buf, SHM_FRAME_BUF_SIZE);
            
            if (raw_len > 0 && raw_buf[0] == 0xFF && raw_buf[1] == 0xD8) {
                pthread_mutex_lock(&frame_mutex);
                size_t copy_len = raw_len;
                if (copy_len > BUFFER_SIZE) copy_len = BUFFER_SIZE;
                memcpy(current_frame, raw_buf, copy_len);
                current_len = copy_len;
                pthread_mutex_unlock(&frame_mutex);
                
                pthread_mutex_lock(&watchdog_mutex);
                if (!first_frame_received) {
                    memcpy(prev_frame, raw_buf, raw_len);
                    prev_frame_len = raw_len;
                    last_frame_time = time(NULL);
                    frame_stuck_count = 0;
                    first_frame_received = 1;
                } else if (raw_len != prev_frame_len || memcmp(raw_buf, prev_frame, raw_len) != 0) {
                    memcpy(prev_frame, raw_buf, raw_len);
                    prev_frame_len = raw_len;
                    last_frame_time = time(NULL);
                    frame_stuck_count = 0;
                    
                    if (watchdog_alert_sent && !camera_restored_alert_sent) {
                        camera_restored_alert_sent = 1;
                        watchdog_alert_sent = 0;
                        send_watchdog_alert("restored");
                        printf("[WATCHDOG] Camera restored!\n");
                    }
                } else {
                    frame_stuck_count++;
                }
                pthread_mutex_unlock(&watchdog_mutex);
            }
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
                printf("[WATCHDOG] No frames for %d seconds!\n", timeout_seconds);
                
                run_command("systemctl restart server.service 2>/dev/null &");
                printf("[WATCHDOG] Server restart triggered\n");
                running = 0;
            }
        }
        else if (frame_stuck_count > 100) {
            if (!watchdog_alert_sent) {
                watchdog_alert_sent = 1;
                camera_restored_alert_sent = 0;
                send_watchdog_alert("stuck");
                printf("[WATCHDOG] Camera STUCK! %d frames unchanged\n", frame_stuck_count);
                
                run_command("systemctl restart server.service 2>/dev/null &");
                printf("[WATCHDOG] Server restart triggered\n");
                running = 0;
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

    if (strcmp(path, "/stream") == 0 || strcmp(path, "/api/v1/stream") == 0) {
        handle_mjpeg_stream(ssl);
        SSL_free(ssl);
        close(fd);
        return NULL;
    }

    if (strcmp(path, "/raw_stream") == 0) {
        int saved_mode = stream_mode;
        stream_mode = 1;
        handle_mjpeg_stream(ssl);
        stream_mode = saved_mode;
        SSL_free(ssl);
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
            SSL_write(ssl, header, strlen(header));
            SSL_write(ssl, copy, len);
            free(copy);
        }
        SSL_free(ssl);
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

        SSL_write(ssl, header, strlen(header));
        SSL_free(ssl);
        close(fd);
        return NULL;
    }

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

    if (strcmp(path, "/api/v1/stream_mode") == 0) {
        if (strcmp(method, "POST") == 0) {
            char *body = strstr(req, "\r\n\r\n");
            if (body) {
                body += 4;
                if (strstr(body, "\"mode\":\"idle\"")) {
                    stream_mode = 0;
                    printf("[STREAM] Mode set to: IDLE (OFF)\n");
                } else if (strstr(body, "\"mode\":\"raw\"")) {
                    stream_mode = 1;
                    printf("[STREAM] Mode set to: RAW\n");
                } else if (strstr(body, "\"mode\":\"processed\"")) {
                    stream_mode = 2;
                    printf("[STREAM] Mode set to: PROCESSED\n");
                }
            }
            const char *mode_str = stream_mode == 0 ? "idle" : (stream_mode == 1 ? "raw" : "processed");
            char resp[256];
            snprintf(resp, sizeof(resp),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Connection: close\r\n"
                "\r\n"
                "{\"mode\":\"%s\"}",
                mode_str);
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

    if (strcmp(path, "/api/v1/stream_mode") == 0) {
        const char *mode_str = stream_mode == 0 ? "idle" : (stream_mode == 1 ? "raw" : "processed");
        char resp[256];
        snprintf(resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Connection: close\r\n"
            "\r\n"
            "{\"mode\":\"%s\"}",
            mode_str);
        SSL_write(ssl, resp, strlen(resp));
        SSL_free(ssl);
        close(fd);
        return NULL;
    }

    if (strcmp(path, "/api/v1/guard") == 0) {
        if (strcmp(method, "POST") == 0) {
            char *body = strstr(req, "\r\n\r\n");
            if (body) {
                body += 4;
                if (strstr(body, "\"enabled\":true")) {
                    guard_enabled = 1;
                    active_detection_event = 0;
                    printf("[GUARD] Guard ENABLED - event state reset\n");
                } else if (strstr(body, "\"enabled\":false")) {
                    guard_enabled = 0;
                    printf("[GUARD] Guard DISABLED\n");
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
                            run_command("shutdown -r now &");
                        } else if (strcmp(command, "shutdown") == 0) {
                            printf("[CMD] Executing shutdown\n");
                            snprintf(json_resp, sizeof(json_resp),
                                     "{\"status\":\"success\",\"cmd\":\"shutdown\"}");
                            run_command("shutdown -h now &");
                        } else if (strcmp(command, "restart_detection") == 0) {
                            printf("[CMD] Restarting detection\n");
                            snprintf(json_resp, sizeof(json_resp),
                                     "{\"status\":\"success\",\"cmd\":\"restart_detection\"}");
                            run_command("systemctl restart detection.service 2>/dev/null &");
                        } else {
                            snprintf(json_resp, sizeof(json_resp),
                                     "{\"status\":\"success\",\"cmd\":\"%s\"}", command);
                            char sys_cmd[128];
                            snprintf(sys_cmd, sizeof(sys_cmd), "%s &", command);
                            run_command(sys_cmd);
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

    char db_path[256];
    if (strlen(g_db_path) > 0) {
        strcpy(db_path, g_db_path);
    } else {
        snprintf(db_path, sizeof(db_path), "%s/security_history.db", getenv("HOME"));
    }
    
    if (history_db_init(db_path) != 0) {
        printf("Failed to initialize SQLite history DB\n");
    } else {
        printf("[SQLITE] Database initialized: %s\n", db_path);
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

    g_processed = processed_frame_open();
    if (!g_processed) {
        printf("[SERVER] Failed to open processed shared memory\n");
    } else {
        printf("[SERVER] Processed shared memory opened\n");
    }

    last_frame_time = time(NULL);

    pthread_create(&history_thread, NULL, history_thread_func, NULL);
    printf("[HISTORY] History thread started\n");

    pthread_create(&email_thread, NULL, email_thread_func, NULL);
    printf("[EMAIL] Email thread started\n");

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
    
    history_thread_running = 0;
    pthread_cond_signal(&history_cond);
    pthread_join(history_thread, NULL);
    printf("[HISTORY] History thread stopped\n");

    email_thread_running = 0;
    pthread_cond_signal(&email_cond);
    pthread_join(email_thread, NULL);
    printf("[EMAIL] Email thread stopped\n");

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