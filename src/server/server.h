#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <microhttpd.h>

#define PORT_HTTP      8080
#define PORT_HTTPS     8443
#define HTML_PATH      "../html/template.html"

/* ---- Telemetry ---- */
float read_cpu_temp(void);
long  read_free_mem(void);
float read_cpu_load(void);

/* ---- HTML template loader ----
 * Returns a malloc'd, NUL-terminated buffer with the template contents.
 * Caller owns the memory (MHD_RESPMEM_MUST_FREE takes care of this when
 * handed straight to MHD_create_response_from_buffer). Returns NULL on
 * failure. */
char *load_html_template(void);

/* ---- MJPEG streaming callback ----
 * Matches MHD_ContentReaderCallback; used with
 * MHD_create_response_from_callback() for the /stream endpoint. */
ssize_t mjpeg_callback(void *cls, uint64_t pos, char *buf, size_t max);

/* ---- Main HTTP/HTTPS request handler ----
 * Matches MHD_AccessHandlerCallback; registered with MHD_start_daemon(). */
int handler(void *cls, struct MHD_Connection *conn,
            const char *url, const char *method,
            const char *version, const char *upload_data,
            size_t *upload_data_size, void **ptr);

#endif /* SERVER_H */