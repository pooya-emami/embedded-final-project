#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "server.h"
#include "shared_frame.h"

// MJPEG frame buffer, shared with the relay process over POSIX shm.
static shared_frame_t *g_frame;

// ---------- Telemetry ----------

float read_cpu_temp() {
    FILE *fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!fp) return -1;
    int temp;
    fscanf(fp, "%d", &temp);
    fclose(fp);
    return temp / 1000.0;
}

long read_free_mem() {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;
    char key[32];
    long value;
    while (fscanf(fp, "%s %ld", key, &value) != EOF) {
        if (strcmp(key, "MemAvailable:") == 0) {
            fclose(fp);
            return value;
        }
    }
    fclose(fp);
    return -1;
}

float read_cpu_load() {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1;

    long user, nice, system, idle;
    fscanf(fp, "cpu %ld %ld %ld %ld", &user, &nice, &system, &idle);
    fclose(fp);

    static long prev_total = 0, prev_idle = 0;
    long total = user + nice + system + idle;

    long diff_total = total - prev_total;
    long diff_idle  = idle - prev_idle;

    prev_total = total;
    prev_idle = idle;

    if (diff_total == 0) return 0;
    return (1.0f - ((float)diff_idle / diff_total)) * 100.0f;
}

// ---------- HTML loader ----------

char *load_html_template() {
    FILE *fp = fopen(HTML_PATH, "r");
    if (!fp) {
        fprintf(stderr, "Failed to open HTML template: %s\n", HTML_PATH);
        return NULL;
    }
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

// ---------- MJPEG callback ----------

ssize_t mjpeg_callback(void *cls, uint64_t pos,
                        char *buf, size_t max)
{
    (void)cls;
    (void)pos;

    static unsigned char frame_buffer[SHM_FRAME_BUF_SIZE];
    static size_t current_len = 0;
    static int has_frame = 0;
    
    if (!has_frame) {
        current_len = shared_frame_read(g_frame, frame_buffer, SHM_FRAME_BUF_SIZE);
        if (current_len == 0) {
            usleep(10000);
            return 0;
        }
        has_frame = 1;
    }
    
    char header[128];
    int header_len = snprintf(header, sizeof(header),
        "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
        current_len);
    
    size_t total_len = header_len + current_len + 2;
    
    if (total_len > max) {
        return -1;
    }
    
    memcpy(buf, header, header_len);
    memcpy(buf + header_len, frame_buffer, current_len);
    buf[header_len + current_len] = '\r';
    buf[header_len + current_len + 1] = '\n';
    
    has_frame = 0;
    return total_len;
}

// ---------- Handler ----------

int handler(void *cls, struct MHD_Connection *conn,
            const char *url, const char *method,
            const char *version, const char *upload_data,
            size_t *upload_data_size, void **ptr)
{
    (void)cls;
    (void)method;
    (void)version;
    (void)upload_data;
    (void)upload_data_size;
    (void)ptr;

    // Serve HTML page
    if (strcmp(url, "/") == 0) {
        char *html = load_html_template();
        if (!html) {
            const char *err = "<html><body>Template error</body></html>";
            struct MHD_Response *resp =
                MHD_create_response_from_buffer(strlen(err),
                                                (void*)err,
                                                MHD_RESPMEM_PERSISTENT);
            return MHD_queue_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, resp);
        }

        struct MHD_Response *resp =
            MHD_create_response_from_buffer(strlen(html),
                                            html,
                                            MHD_RESPMEM_MUST_FREE);
        MHD_add_response_header(resp, "Content-Type", "text/html");
        return MHD_queue_response(conn, MHD_HTTP_OK, resp);
    }

    // Telemetry JSON
    if (strcmp(url, "/telemetry") == 0) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"temp\":%.2f,\"mem\":%ld,\"cpu\":%.2f}",
                 read_cpu_temp(), read_free_mem(), read_cpu_load());

        struct MHD_Response *resp =
            MHD_create_response_from_buffer(strlen(buf),
                                            buf,
                                            MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        return MHD_queue_response(conn, MHD_HTTP_OK, resp);
    }

    // MJPEG stream
    if (strcmp(url, "/stream") == 0) {
        struct MHD_Response *resp =
            MHD_create_response_from_callback(
                MHD_SIZE_UNKNOWN,
                4096,
                &mjpeg_callback,
                NULL,
                NULL);

        MHD_add_response_header(resp, "Cache-Control", "no-cache, no-store, must-revalidate");
        MHD_add_response_header(resp, "Pragma", "no-cache");
        MHD_add_response_header(resp, "Expires", "0");
        MHD_add_response_header(resp, "Connection", "close");
        MHD_add_response_header(resp, "Content-Type",
            "multipart/x-mixed-replace; boundary=frame");

        return MHD_queue_response(conn, MHD_HTTP_OK, resp);
    }

    // 404
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT);
    return MHD_queue_response(conn, MHD_HTTP_NOT_FOUND, resp);
}

// ---------- main ----------

int main() {
    // Attach to the shared frame buffer
    g_frame = shared_frame_open();
    if (!g_frame) {
        fprintf(stderr, "Failed to open shared frame buffer\n");
        return 1;
    }

    // Load SSL certs
    FILE *fkey = fopen("key.pem", "rb");
    FILE *fcert = fopen("cert.pem", "rb");
    if (!fkey || !fcert) {
        fprintf(stderr, "Missing SSL cert.pem/key.pem\n");
        return 1;
    }

    fseek(fkey, 0, SEEK_END);
    size_t key_len = ftell(fkey);
    fseek(fkey, 0, SEEK_SET);
    char *key_buf = malloc(key_len);
    fread(key_buf, 1, key_len, fkey);

    fseek(fcert, 0, SEEK_END);
    size_t cert_len = ftell(fcert);
    fseek(fcert, 0, SEEK_SET);
    char *cert_buf = malloc(cert_len);
    fread(cert_buf, 1, cert_len, fcert);

    fclose(fkey);
    fclose(fcert);

    struct MHD_Daemon *daemon =
        MHD_start_daemon(MHD_USE_SELECT_INTERNALLY | MHD_USE_SSL,
                         PORT_HTTPS,
                         NULL, NULL,
                         &handler, NULL,
                         MHD_OPTION_HTTPS_MEM_KEY, key_buf,
                         MHD_OPTION_HTTPS_MEM_CERT, cert_buf,
                         MHD_OPTION_END);

    if (!daemon) {
        fprintf(stderr, "Failed to start HTTPS server\n");
        return 1;
    }

    printf("HTTPS server running on port %d\n", PORT_HTTPS);
    printf("Open: https://192.168.137.100:8443/\n");
    
    getchar();
    MHD_stop_daemon(daemon);

    free(key_buf);
    free(cert_buf);
    return 0;
}