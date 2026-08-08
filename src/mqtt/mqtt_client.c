#include "mqtt_client.h"
#include <mosquitto.h>
#include <stdio.h>
#include <time.h>

static struct mosquitto *mosq = NULL;
static const char *student_id = "404300409";

void mqtt_init(const char *host, int port, const char *user, const char *pass)
{
    mosquitto_lib_init();
    mosq = mosquitto_new(student_id, true, NULL);

    mosquitto_username_pw_set(mosq, user, pass);

    mosquitto_will_set(
        mosq,
        "telemetry/404300409/home",
        22,
        "{\"status\":\"offline\"}",
        1,
        false
    );

    if (mosquitto_connect(mosq, host, port, 60) != MOSQ_ERR_SUCCESS) {
        printf("[MQTT] Connection failed\n");
        return;
    }

    mosquitto_loop_start(mosq);
    printf("[MQTT] Connected\n");
}

void mqtt_publish_persons(int count, float temp)
{
    char msg[128];
    snprintf(msg, sizeof(msg),
             "{\"count\":%d,\"temp\":%.1f,\"timestamp\":%ld}",
             count, temp, time(NULL));

    mosquitto_publish(
        mosq, NULL,
        "persons/404300409/home",
        strlen(msg), msg,
        1, false
    );
}

void mqtt_publish_telemetry(float temp, long mem, float cpu)
{
    char msg[128];
    snprintf(msg, sizeof(msg),
             "{\"temp\":%.1f,\"mem\":%ld,\"cpu\":%.1f,\"timestamp\":%ld}",
             temp, mem, cpu, time(NULL));

    mosquitto_publish(
        mosq, NULL,
        "telemetry/404300409/home",
        strlen(msg), msg,
        1, false
    );
}

void mqtt_publish_alarm(int count, float temp)
{
    char msg[128];
    snprintf(msg, sizeof(msg),
             "{\"count\":%d,\"temp\":%.1f,\"timestamp\":%ld}",
             count, temp, time(NULL));

    mosquitto_publish(
        mosq, NULL,
        "alarm/404300409/home",
        strlen(msg), msg,
        1, false
    );
}

void mqtt_cleanup(void)
{
    if (mosq) {
        mosquitto_disconnect(mosq);
        mosquitto_loop_stop(mosq, true);
        mosquitto_destroy(mosq);
    }
    mosquitto_lib_cleanup();
}
