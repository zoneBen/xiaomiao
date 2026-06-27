#ifndef NETWORK_H
#define NETWORK_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_SSID_LENGTH 32
#define MAX_PASSWORD_LENGTH 64
#define MAX_MQTT_URI_LENGTH 128
#define MAX_CLIENT_ID_LENGTH 64
#define MAX_TOPIC_LENGTH 128

typedef enum {
    NETWORK_STATE_DISCONNECTED,
    NETWORK_STATE_CONNECTING_WIFI,
    NETWORK_STATE_WIFI_CONNECTED,
    NETWORK_STATE_CONNECTING_MQTT,
    NETWORK_STATE_MQTT_CONNECTED,
    NETWORK_STATE_ERROR
} NetworkState;

typedef void (*network_message_callback_t)(const char* topic, const char* payload, size_t length);
typedef void (*network_state_callback_t)(NetworkState state);

void network_init(void);
void network_deinit(void);

bool network_set_wifi_credentials(const char* ssid, const char* password);
bool network_set_mqtt_config(const char* uri, const char* client_id, const char* username, const char* password);

bool network_connect(void);
void network_disconnect(void);
bool network_is_connected(void);
NetworkState network_get_state(void);

bool network_mqtt_publish(const char* topic, const char* payload, size_t length);
bool network_mqtt_subscribe(const char* topic);
bool network_mqtt_unsubscribe(const char* topic);

void network_set_message_callback(network_message_callback_t callback);
void network_set_state_callback(network_state_callback_t callback);

const char* network_get_ip_address(void);

#endif
