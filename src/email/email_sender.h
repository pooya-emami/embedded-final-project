#ifndef EMAIL_SENDER_H
#define EMAIL_SENDER_H

#include <stddef.h>

extern char g_smtp_server[128];
extern char g_smtp_user[128];
extern char g_smtp_pass[128];
extern char g_smtp_to[128];

int email_send_alert(int persons, float cpu_temp, 
                     const unsigned char *jpeg_buf, size_t jpeg_len);

int email_send_alert_guard(int persons, float cpu_temp,
                           const unsigned char *jpeg_buf, size_t jpeg_len);

int email_send_alert_watchdog(float cpu_temp);

#endif