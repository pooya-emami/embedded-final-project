#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stddef.h>

#define STUDENT_ID "404300409"
#define CLIENTID "pooya_security_client"

void mqtt_init(const char *host, int port, 
               const char *user, const char *pass);

void mqtt_publish_persons(int count, float temp);
void mqtt_publish_telemetry(float temp, long mem, float cpu);
void mqtt_publish_alarm(int count, float temp);
void mqtt_publish_custom(const char *topic, const char *payload);
void mqtt_cleanup(void);

#endif