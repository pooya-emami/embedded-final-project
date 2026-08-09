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
static time_t last_watchdog_email_time = 0;


static int send_email_with_subject(const char *subject, const char *body_prefix,
                                   int persons, float cpu_temp,
                                   const unsigned char *jpeg_buf, size_t jpeg_len)
{
    time_t now = time(NULL);
    
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

    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part;

    // Email body
    char email_body[1024];
    time_t now_time = time(NULL);
    struct tm *tm_info = localtime(&now_time);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    snprintf(email_body, sizeof(email_body),
        "%s\n"
        "========================================\n"
        "Timestamp: %s\n"
        "Persons Detected: %d\n"
        "CPU Temperature: %.1f C\n"
        "========================================\n\n"
        "This is an automated alert from the Embedded Security System.\n",
        body_prefix, timestamp, persons, cpu_temp);

    // Subject header for email
    char subject_header[256];
    snprintf(subject_header, sizeof(subject_header), "Subject: %s\r\n", subject);

    // Combine subject + body
    char full_message[2048];
    snprintf(full_message, sizeof(full_message), "%s\r\n%s", subject_header, email_body);

    part = curl_mime_addpart(mime);
    curl_mime_data(part, full_message, CURL_ZERO_TERMINATED);
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
        printf("[EMAIL] Alert sent successfully: %s\n", subject);
        last_email_time = now;
    } else {
        printf("[EMAIL] Failed to send: %s\n", curl_easy_strerror(res));
    }

    curl_slist_free_all(recipients);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) ? 0 : -1;
}

static int send_watchdog_tamper_email(float cpu_temp)
{
    time_t now = time(NULL);
    
    // Watchdog debounce: at most 1 email per 30 seconds
    if (now - last_watchdog_email_time < 30) {
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

    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part;

    // Email body
    char email_body[1024];
    time_t now_time = time(NULL);
    struct tm *tm_info = localtime(&now_time);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    snprintf(email_body, sizeof(email_body),
        "WATCHDOG: Camera TAMPERING detected!\n"
        "========================================\n"
        "Timestamp: %s\n"
        "CPU Temperature: %.1f C\n"
        "========================================\n\n"
        "The camera feed has been interrupted or tampered with!\n"
        "Possible issues: Camera disconnected, cable cut, power loss, or system freeze.\n\n"
        "This is an automated alert from the Embedded Security System.\n",
        timestamp, cpu_temp);

    char full_message[2048];
    snprintf(full_message, sizeof(full_message), 
        "Subject: WATCHDOG ALERT: Camera TAMPERING Detected!\r\n\r\n%s", email_body);

    part = curl_mime_addpart(mime);
    curl_mime_data(part, full_message, CURL_ZERO_TERMINATED);
    curl_mime_type(part, "text/plain");

    // JSON part
    char json_body[256];
    snprintf(json_body, sizeof(json_body),
        "{\"timestamp\":%ld,\"status\":\"camera_tampered\",\"cpu_temp\":%.2f}",
        now_time, cpu_temp);
    part = curl_mime_addpart(mime);
    curl_mime_data(part, json_body, CURL_ZERO_TERMINATED);
    curl_mime_type(part, "application/json");

    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        printf("[EMAIL] Watchdog tamper alert sent\n");
        last_watchdog_email_time = now;
    } else {
        printf("[EMAIL] Failed to send watchdog alert: %s\n", curl_easy_strerror(res));
    }

    curl_slist_free_all(recipients);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) ? 0 : -1;
}

int email_send_alert(int persons, float cpu_temp,
                     const unsigned char *jpeg_buf, size_t jpeg_len)
{
    // Debounce: at most 1 email per 30 seconds
    time_t now = time(NULL);
    if (now - last_email_time < 30) {
        return 0;
    }
    
    return send_email_with_subject(
        "NORMAL ALERT: Person Detected",
        "NORMAL MODE: Person(s) detected in camera feed.",
        persons, cpu_temp, jpeg_buf, jpeg_len
    );
}

int email_send_alert_guard(int persons, float cpu_temp,
                           const unsigned char *jpeg_buf, size_t jpeg_len)
{
    return send_email_with_subject(
        "URGENT! Guard Mode Alert - Person Detected",
        "GUARD MODE: Person(s) detected in camera feed! Immediate attention required.",
        persons, cpu_temp, jpeg_buf, jpeg_len
    );
}

int email_send_alert_watchdog(float cpu_temp)
{
    return send_watchdog_tamper_email(cpu_temp);
}