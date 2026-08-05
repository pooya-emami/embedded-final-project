#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define PORT_HTTP   8080
#define PORT_HTTPS  8443
#define HTML_PATH   "../html/template.html"
#define BUFFER_SIZE 65536

float read_cpu_temp(void);
long  read_free_mem(void);
float read_cpu_load(void);

void send_response(int client_fd, const char *status,
                   const char *content_type,
                   const void *data, size_t data_len);

char *load_html_template(void);

void send_frame(int client_fd);
void send_html(int client_fd);
void send_telemetry(int client_fd);

void *frame_updater(void *arg);

#endif