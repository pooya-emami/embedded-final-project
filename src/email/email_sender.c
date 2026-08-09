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
    printf("[EMAIL] 🔵 FUNCTION ENTERED! persons=%d, temp=%.1f, jpeg_len=%zu\n", 
           persons, cpu_temp, jpeg_len);
    
    time_t now = time(NULL);
    
    // Debounce check
    if (now - last_email_time < 30) {
        printf("[EMAIL] Debounced: %lds since last email\n", now - last_email_time);
        return 0;
    }
    
    // Print SMTP config (debug)
    printf("[EMAIL] SMTP Config:\n");
    printf("[EMAIL]   SERVER: '%s'\n", g_smtp_server);
    printf("[EMAIL]   USER: '%s'\n", g_smtp_user);
    printf("[EMAIL]   PASS: '%s'\n", g_smtp_pass[0] ? "***SET***" : "NOT SET");
    printf("[EMAIL]   TO: '%s'\n", g_smtp_to);
    
    // Check if SMTP is configured
    if (strlen(g_smtp_server) == 0 || strlen(g_smtp_user) == 0 || 
        strlen(g_smtp_pass) == 0 || strlen(g_smtp_to) == 0) {
        printf("[EMAIL] ❌ SMTP not configured!\n");
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        printf("[EMAIL] ❌ Failed to initialize curl\n");
        return -1;
    }

    printf("[EMAIL] Curl initialized, sending email...\n");

    curl_easy_setopt(curl, CURLOPT_USERNAME, g_smtp_user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, g_smtp_pass);
    curl_easy_setopt(curl, CURLOPT_URL, g_smtp_server);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, g_smtp_user);
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    
    // Enable verbose output to see SMTP conversation
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    struct curl_slist *recipients = NULL;
    recipients = curl_slist_append(recipients, g_smtp_to);
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);

    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part;

    // TEXT PART
    part = curl_mime_addpart(mime);
    char header_text[256];
    snprintf(header_text, sizeof(header_text), 
             "Security Alert - Human Detected! (%d persons)\n", persons);
    curl_mime_data(part, header_text, CURL_ZERO_TERMINATED);

    // JSON INFO PART
    char info[256];
    snprintf(info, sizeof(info),
        "{\"timestamp\":%ld,\"persons\":%d,\"cpu_temp\":%.2f}\n",
        now, persons, cpu_temp);
    part = curl_mime_addpart(mime);
    curl_mime_data(part, info, CURL_ZERO_TERMINATED);
    curl_mime_type(part, "application/json");

    // JPEG ATTACHMENT
    if (jpeg_buf && jpeg_len > 0) {
        printf("[EMAIL] Attaching JPEG: %zu bytes\n", jpeg_len);
        part = curl_mime_addpart(mime);
        curl_mime_data(part, (const char *)jpeg_buf, jpeg_len);
        curl_mime_filename(part, "alert.jpg");
        curl_mime_type(part, "image/jpeg");
    } else {
        printf("[EMAIL] No JPEG attachment\n");
    }

    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    printf("[EMAIL] Performing curl request...\n");
    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        printf("[EMAIL] ❌ Curl error: %s\n", curl_easy_strerror(res));
    } else {
        printf("[EMAIL] ✅ Alert sent successfully!\n");
        last_email_time = now;
    }

    curl_slist_free_all(recipients);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) ? 0 : -1;
}