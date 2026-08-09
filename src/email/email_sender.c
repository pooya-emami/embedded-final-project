#include <curl/curl.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
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
        printf("[EMAIL] Debounced: %lds since last email\n", now - last_email_time);
        return 0;
    }
    last_email_time = now;

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    curl_easy_setopt(curl, CURLOPT_USERNAME, g_smtp_user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, g_smtp_pass);
    curl_easy_setopt(curl, CURLOPT_URL, g_smtp_server);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, g_smtp_user);

    struct curl_slist *recipients = NULL;
    recipients = curl_slist_append(recipients, g_smtp_to);
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);

    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part;

    // TEXT PART - alert header
    part = curl_mime_addpart(mime);
    curl_mime_data(part, "Security Alert - Human Detected!\n", CURL_ZERO_TERMINATED);

    // JSON INFO PART
    char info[256];
    snprintf(info, sizeof(info),
        "{\"timestamp\":%ld,\"persons\":%d,\"cpu_temp\":%.2f}\n",
        now, persons, cpu_temp);
    part = curl_mime_addpart(mime);
    curl_mime_data(part, info, CURL_ZERO_TERMINATED);
    curl_mime_type(part, "application/json");

    // JPEG ATTACHMENT (if provided)
    if (jpeg_buf && jpeg_len > 0) {
        part = curl_mime_addpart(mime);
        curl_mime_data(part, (const char *)jpeg_buf, jpeg_len);
        curl_mime_filename(part, "alert.jpg");
        curl_mime_type(part, "image/jpeg");
    }

    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(recipients);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) ? 0 : -1;
}