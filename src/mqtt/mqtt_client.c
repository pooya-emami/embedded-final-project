#include "mqtt_client.h"
#include "MQTTClient.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define QOS         1
#define TIMEOUT     10000L

static MQTTClient client;
static int mqtt_connected = 0;

void mqtt_init(const char *host, int port, const char *user, const char *pass)
{
    char address[128];
    snprintf(address, sizeof(address), "tcp://%s:%d", host, port);

    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    MQTTClient_willOptions will_opts = MQTTClient_willOptions_initializer;

    // LWT on telemetry topic - indicates board went offline
    will_opts.topicName = "telemetry/" STUDENT_ID "/home";
    will_opts.message = "{\"status\":\"offline\"}";
    will_opts.qos = QOS;
    will_opts.retained = 0;

    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1;
    conn_opts.will = &will_opts;
    conn_opts.username = user;
    conn_opts.password = pass;

    MQTTClient_create(&client, address, CLIENTID,
                      MQTTCLIENT_PERSISTENCE_NONE, NULL);

    int rc = MQTTClient_connect(client, &conn_opts);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[MQTT] Connect failed: %d\n", rc);
        mqtt_connected = 0;
        return;
    }

    mqtt_connected = 1;
    printf("[MQTT] Connected to %s\n", address);
}

static void mqtt_publish(const char *topic, const char *payload)
{
    if (!mqtt_connected) {
        printf("[MQTT] Not connected, skipping publish\n");
        return;
    }

    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    pubmsg.payload = (void*)payload;
    pubmsg.payloadlen = (int)strlen(payload);
    pubmsg.qos = QOS;
    pubmsg.retained = 0;

    MQTTClient_deliveryToken token;
    MQTTClient_publishMessage(client, topic, &pubmsg, &token);
    MQTTClient_waitForCompletion(client, token, TIMEOUT);
}

void mqtt_publish_persons(int count, float temp)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "persons/" STUDENT_ID "/home");

    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"persons\":%d,\"temp\":%.2f,\"timestamp\":%ld}",
        count, temp, time(NULL));

    mqtt_publish(topic, payload);
}

void mqtt_publish_telemetry(float temp, long mem, float cpu)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "telemetry/" STUDENT_ID "/home");

    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"temp\":%.2f,\"mem\":%ld,\"cpu\":%.2f,\"timestamp\":%ld}",
        temp, mem, cpu, time(NULL));

    mqtt_publish(topic, payload);
}

void mqtt_publish_alarm(int count, float temp)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "alarm/" STUDENT_ID "/home");

    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"persons\":%d,\"temp\":%.2f,\"timestamp\":%ld}",
        count, temp, time(NULL));

    mqtt_publish(topic, payload);
}

void mqtt_cleanup(void)
{
    if (mqtt_connected) {
        MQTTClient_disconnect(client, 1000);
        MQTTClient_destroy(&client);
    }
}