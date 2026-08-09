#ifndef EMAIL_SENDER_H
#define EMAIL_SENDER_H

#include <stddef.h>

// SMTP configuration - set by server.c
extern char g_smtp_server[128];
extern char g_smtp_user[128];
extern char g_smtp_pass[128];
extern char g_smtp_to[128];

// Returns 0 on success, -1 on failure
int email_send_alert(int persons, float cpu_temp, 
                     const unsigned char *jpeg_buf, size_t jpeg_len);

// Guard mode email with different subject/body
int email_send_alert_guard(int persons, float cpu_temp,
                           const unsigned char *jpeg_buf, size_t jpeg_len);

// Watchdog tamper alert email
int email_send_alert_watchdog(float cpu_temp);

#endif