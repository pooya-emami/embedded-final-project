#include "email_sender.h"
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static time_t last_email_time = 0;

void email_send_alert(int count, float temp, const unsigned char *jpeg, size_t len)
{
    time_t now = time(NULL);
    if (now - last_email_time < 30) {
        printf("[EMAIL] Debounced\n");
        return;
    }
    last_email_time = now;

    CURL *curl = curl_easy_init();
    if (!curl) return;

    struct curl_slist *recipients = NULL;
    recipients = curl_slist_append(recipients, "your_email@example.com");

    curl_easy_setopt(curl, CURLOPT_USERNAME, "smtp_user");
    curl_easy_setopt(curl, CURLOPT_PASSWORD, "smtp_pass");
    curl_easy_setopt(curl, CURLOPT_URL, "smtp://smtp.gmail.com:587");
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);

    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, "<your_email@example.com>");
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);

    char body[512];
    snprintf(body, sizeof(body),
             "Subject: ALERT: Person detected\n"
             "From: Security System\n"
             "To: you\n\n"
             "Count: %d\nTemp: %.1f\nTime: %ld\n",
             count, temp, now);

    curl_easy_setopt(curl, CURLOPT_READFUNCTION, NULL);
    curl_easy_setopt(curl, CURLOPT_READDATA, body);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

    curl_easy_perform(curl);

    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);

    printf("[EMAIL] Sent alert\n");
}
