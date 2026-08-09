#include <curl/curl.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "email_sender.h"

// These are defined in server.c
extern char g_smtp_server[128];
extern char g_smtp_user[128];
extern char g_smtp_pass[128];
extern char g_smtp_to[128];

static time_t last_email_time = 0;

int email_send_alert(int persons, float cpu_temp,
                     const unsigned char *jpeg_buf, size_t jpeg_len)
{
    time_t now = time(NULL);
    
    // Debounce: at most 1 email per 30 seconds
    if (now - last_email_time < 30) {
        return 0;
    }
    
    // Check SMTP config
    if (strlen(g_smtp_server) == 0 || strlen(g_smtp_user) == 0 || 
        strlen(g_smtp_pass) == 0 || strlen(g_smtp_to) == 0) {
        printf("[EMAIL] SMTP not configured\n");
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        printf("[EMAIL] Failed to initialize curl\n");
        return -1;
    }

    // Setup SMTP
    curl_easy_setopt(curl, CURLOPT_USERNAME, g_smtp_user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, g_smtp_pass);
    curl_easy_setopt(curl, CURLOPT_URL, g_smtp_server);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, g_smtp_user);
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    struct curl_slist *recipients = NULL;
    recipients = curl_slist_append(recipients, g_smtp_to);
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);

    // Create MIME message
    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part;

    // Email body
    char email_body[512];
    time_t now_time = time(NULL);
    struct tm *tm_info = localtime(&now_time);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    snprintf(email_body, sizeof(email_body),
        "Security Alert - Human Detected!\n"
        "Timestamp: %s\n"
        "Persons: %d\n"
        "CPU Temp: %.1f C\n\n"
        "Attached: alert.jpg\n",
        timestamp, persons, cpu_temp);

    part = curl_mime_addpart(mime);
    curl_mime_data(part, email_body, CURL_ZERO_TERMINATED);
    curl_mime_type(part, "text/plain");

    // JSON part
    char json_body[256];
    snprintf(json_body, sizeof(json_body),
        "{\"timestamp\":%ld,\"persons\":%d,\"cpu_temp\":%.2f}",
        now_time, persons, cpu_temp);
    part = curl_mime_addpart(mime);
    curl_mime_data(part, json_body, CURL_ZERO_TERMINATED);
    curl_mime_type(part, "application/json");

    // JPEG attachment (if provided)
    if (jpeg_buf && jpeg_len > 100) {
        if (jpeg_buf[0] == 0xFF && jpeg_buf[1] == 0xD8) {
            part = curl_mime_addpart(mime);
            curl_mime_data(part, (const char *)jpeg_buf, jpeg_len);
            curl_mime_filename(part, "alert.jpg");
            curl_mime_type(part, "image/jpeg");
            curl_mime_encoder(part, "base64");
        }
    }

    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        printf("[EMAIL] Alert sent successfully\n");
        last_email_time = now;
    } else {
        printf("[EMAIL] Failed to send: %s\n", curl_easy_strerror(res));
    }

    curl_slist_free_all(recipients);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) ? 0 : -1;
}