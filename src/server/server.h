#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define BUFFER_SIZE 65536
#define HTML_PATH "../html/template.html"
#define CONFIG_PATH "server.conf"

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

// SMTP config (accessible to email_sender.c)
extern char g_smtp_server[128];
extern char g_smtp_user[128];
extern char g_smtp_pass[128];
extern char g_smtp_to[128];

// Functions
void load_config(const char *filename);
void reload_config(const char *filename);
void send_redirect(int fd, const char *host_header);
void *frame_updater(void *arg);

// History management
typedef struct {
    int count;
    time_t timestamp;
    float temp;
} detection_record_t;

void add_history(int count, float temp);

#endif /* SERVER_H */