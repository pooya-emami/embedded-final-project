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

#endif