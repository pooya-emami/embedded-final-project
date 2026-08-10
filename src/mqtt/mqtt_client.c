#include "mqtt_client.h"
#include <MQTTClient.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define QOS         1
#define TIMEOUT     5000L

static MQTTClient client;
static int mqtt_connected = 0;
static char mqtt_address[128] = {0};
static char mqtt_user[64] = {0};
static char mqtt_pass[64] = {0};
static time_t last_reconnect_attempt = 0;
static int reconnect_interval = 10;

static void mqtt_publish(const char *topic, const char *payload)
{
    if (!mqtt_connected) {
        // Try to reconnect
        time_t now = time(NULL);
        if (now - last_reconnect_attempt < reconnect_interval) {
            printf("[MQTT] Not connected, skipping publish\n");
            return;
        }
        last_reconnect_attempt = now;

        MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
        conn_opts.keepAliveInterval = 20;
        conn_opts.cleansession = 1;
        conn_opts.connectTimeout = 5;
        conn_opts.username = mqtt_user;
        conn_opts.password = mqtt_pass;

        printf("[MQTT] Attempting reconnect...\n");
        int rc = MQTTClient_connect(client, &conn_opts);
        if (rc == MQTTCLIENT_SUCCESS) {
            mqtt_connected = 1;
            printf("[MQTT] Reconnected successfully\n");
        } else {
            mqtt_connected = 0;
            printf("[MQTT] Reconnect failed: %d\n", rc);
            return;
        }
    }

    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    pubmsg.payload = (void*)payload;
    pubmsg.payloadlen = (int)strlen(payload);
    pubmsg.qos = QOS;
    pubmsg.retained = 0;

    MQTTClient_deliveryToken token;
    int rc = MQTTClient_publishMessage(client, topic, &pubmsg, &token);
    
    if (rc != MQTTCLIENT_SUCCESS) {
        mqtt_connected = 0;
        printf("[MQTT] Publish failed: %d\n", rc);
        return;
    }
    
    rc = MQTTClient_waitForCompletion(client, token, TIMEOUT);
    if (rc != MQTTCLIENT_SUCCESS) {
        mqtt_connected = 0;
        printf("[MQTT] Wait failed: %d\n", rc);
    }
}

void mqtt_init(const char *host, int port, const char *user, const char *pass)
{
    snprintf(mqtt_address, sizeof(mqtt_address), "tcp://%s:%d", host, port);
    strncpy(mqtt_user, user, sizeof(mqtt_user) - 1);
    strncpy(mqtt_pass, pass, sizeof(mqtt_pass) - 1);

    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    MQTTClient_willOptions will_opts = MQTTClient_willOptions_initializer;

    will_opts.topicName = "telemetry/" STUDENT_ID "/home";
    will_opts.message = "{\"status\":\"offline\"}";
    will_opts.qos = QOS;
    will_opts.retained = 0;

    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1;
    conn_opts.will = &will_opts;
    conn_opts.username = user;
    conn_opts.password = pass;
    conn_opts.connectTimeout = 5;

    int rc = MQTTClient_create(&client, mqtt_address, CLIENTID,
                               MQTTCLIENT_PERSISTENCE_NONE, NULL);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[MQTT] Create failed: %d\n", rc);
        mqtt_connected = 0;
        return;
    }

    rc = MQTTClient_connect(client, &conn_opts);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[MQTT] Connect failed: %d\n", rc);
        mqtt_connected = 0;
    } else {
        mqtt_connected = 1;
        printf("[MQTT] Connected to %s\n", mqtt_address);
    }
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

void mqtt_publish_custom(const char *topic, const char *payload)
{
    mqtt_publish(topic, payload);
}

void mqtt_cleanup(void)
{
    if (mqtt_connected) {
        MQTTClient_disconnect(client, 1000);
        MQTTClient_destroy(&client);
        mqtt_connected = 0;
    }
}