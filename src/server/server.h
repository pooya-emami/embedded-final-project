#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define PORT_HTTP   8080
#define PORT_HTTPS  8443
#define HTML_PATH   "../html/template.html"
#define BUFFER_SIZE 65536
#define MAX_HISTORY 5

// Telemetry functions
float read_cpu_temp(void);
long  read_free_mem(void);
float read_cpu_load(void);

// HTTP response helpers
void send_response(int client_fd, const char *status,
                   const char *content_type,
                   const void *data, size_t data_len);

void send_redirect(int fd);

// HTML template loader
char *load_html_template(void);

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