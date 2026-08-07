#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define CONFIG_PATH "/usr/local/bin/server.conf"

// Default values (used if config file not found)
#define DEFAULT_FRAME_INTERVAL_MS 100
#define DEFAULT_FRAME_WIDTH 1280
#define DEFAULT_FRAME_HEIGHT 720
#define DEFAULT_PORT_HTTP 8080
#define DEFAULT_PORT_HTTPS 8443
#define DEFAULT_MAX_HISTORY 5
#define DEFAULT_TEMP_THROTTLE_C 65
#define DEFAULT_MIN_INTERVAL_MS 200
#define DEFAULT_WATCHDOG_TIMEOUT_MS 100

// Global config variables
extern int g_frame_interval_ms;
extern int g_frame_width;
extern int g_frame_height;
extern int g_port_http;
extern int g_port_https;
extern int g_max_history;
extern int g_temp_throttle_c;
extern int g_min_interval_ms;
extern int g_watchdog_timeout_ms;

// Functions
void load_config(const char *filename);
void reload_config(const char *filename);

// Telemetry functions
float read_cpu_temp(void);
long  read_free_mem(void);
float read_cpu_load(void);

// Frame functions
void send_frame(int client_fd);
void send_html(int client_fd);
void send_telemetry(int client_fd);

// Frame updater thread
void *frame_updater(void *arg);

// History management
typedef struct {
    int count;
    time_t timestamp;
    float temp;
} detection_record_t;

void add_history(int count, float temp);

#endif /* SERVER_H */