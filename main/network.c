#include "network.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include <string.h>

static const char* TAG = "chess_network";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MQTT_CONNECTED_BIT BIT2

typedef struct {
    char ssid[MAX_SSID_LENGTH];
    char password[MAX_PASSWORD_LENGTH];
    char mqtt_uri[MAX_MQTT_URI_LENGTH];
    char mqtt_client_id[MAX_CLIENT_ID_LENGTH];
    char mqtt_username[MAX_CLIENT_ID_LENGTH];
    char mqtt_password[MAX_PASSWORD_LENGTH];
    char ip_address[16];
    NetworkState state;
    bool initialized;
    EventGroupHandle_t event_group;
    esp_mqtt_client_handle_t mqtt_client;
    network_message_callback_t message_callback;
    network_state_callback_t state_callback;
} NetworkContext;

static NetworkContext* ctx = NULL;

static void update_state(NetworkState new_state) {
    if (ctx && ctx->state != new_state) {
        ctx->state = new_state;
        if (ctx->state_callback) {
            ctx->state_callback(new_state);
        }
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        update_state(NETWORK_STATE_CONNECTING_WIFI);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(ctx->event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(ctx->event_group, WIFI_FAIL_BIT);
        esp_wifi_connect();
        update_state(NETWORK_STATE_CONNECTING_WIFI);
        ESP_LOGI(TAG, "Retry connecting to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(ctx->ip_address, sizeof(ctx->ip_address),
                 IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", ctx->ip_address);
        xEventGroupSetBits(ctx->event_group, WIFI_CONNECTED_BIT);
        xEventGroupClearBits(ctx->event_group, WIFI_FAIL_BIT);
        update_state(NETWORK_STATE_WIFI_CONNECTED);
    }
}

static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                               int32_t event_id, void* event_data) {
    esp_mqtt_event_handle_t event = event_data;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected");
            xEventGroupSetBits(ctx->event_group, MQTT_CONNECTED_BIT);
            update_state(NETWORK_STATE_MQTT_CONNECTED);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT Disconnected");
            xEventGroupClearBits(ctx->event_group, MQTT_CONNECTED_BIT);
            update_state(NETWORK_STATE_WIFI_CONNECTED);
            break;
        case MQTT_EVENT_DATA:
            if (ctx->message_callback) {
                ctx->message_callback(event->topic, event->data, event->data_len);
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT Error");
            update_state(NETWORK_STATE_ERROR);
            break;
        default:
            break;
    }
}

void network_init(void) {
    if (ctx) {
        return;
    }

    ctx = (NetworkContext*)calloc(1, sizeof(NetworkContext));
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to allocate network context");
        return;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ctx->event_group = xEventGroupCreate();
    ctx->state = NETWORK_STATE_DISCONNECTED;
    ctx->initialized = true;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));
}

void network_deinit(void) {
    if (!ctx) {
        return;
    }

    network_disconnect();

    if (ctx->event_group) {
        vEventGroupDelete(ctx->event_group);
    }

    free(ctx);
    ctx = NULL;
}

bool network_set_wifi_credentials(const char* ssid, const char* password) {
    if (!ctx || !ssid) {
        return false;
    }

    strncpy(ctx->ssid, ssid, sizeof(ctx->ssid) - 1);
    ctx->ssid[sizeof(ctx->ssid) - 1] = '\0';

    if (password) {
        strncpy(ctx->password, password, sizeof(ctx->password) - 1);
        ctx->password[sizeof(ctx->password) - 1] = '\0';
    } else {
        ctx->password[0] = '\0';
    }

    return true;
}

bool network_set_mqtt_config(const char* uri, const char* client_id,
                             const char* username, const char* password) {
    if (!ctx || !uri) {
        return false;
    }

    strncpy(ctx->mqtt_uri, uri, sizeof(ctx->mqtt_uri) - 1);
    ctx->mqtt_uri[sizeof(ctx->mqtt_uri) - 1] = '\0';

    if (client_id) {
        strncpy(ctx->mqtt_client_id, client_id, sizeof(ctx->mqtt_client_id) - 1);
        ctx->mqtt_client_id[sizeof(ctx->mqtt_client_id) - 1] = '\0';
    }

    if (username) {
        strncpy(ctx->mqtt_username, username, sizeof(ctx->mqtt_username) - 1);
        ctx->mqtt_username[sizeof(ctx->mqtt_username) - 1] = '\0';
    }

    if (password) {
        strncpy(ctx->mqtt_password, password, sizeof(ctx->mqtt_password) - 1);
        ctx->mqtt_password[sizeof(ctx->mqtt_password) - 1] = '\0';
    }

    return true;
}

bool network_connect(void) {
    if (!ctx || !ctx->initialized) {
        return false;
    }

    if (strlen(ctx->ssid) == 0) {
        ESP_LOGE(TAG, "WiFi SSID not set");
        return false;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    strncpy((char*)wifi_config.sta.ssid, ctx->ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (strlen(ctx->password) > 0) {
        strncpy((char*)wifi_config.sta.password, ctx->password, sizeof(wifi_config.sta.password) - 1);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(ctx->event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE,
                                            pdFALSE,
                                            pdMS_TO_TICKS(10000));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "WiFi connection failed");
        update_state(NETWORK_STATE_ERROR);
        return false;
    }

    if (strlen(ctx->mqtt_uri) > 0) {
        update_state(NETWORK_STATE_CONNECTING_MQTT);

        esp_mqtt_client_config_t mqtt_cfg = {
            .broker.address.uri = ctx->mqtt_uri,
        };

        if (strlen(ctx->mqtt_client_id) > 0) {
            mqtt_cfg.credentials.client_id = ctx->mqtt_client_id;
        }
        if (strlen(ctx->mqtt_username) > 0) {
            mqtt_cfg.credentials.username = ctx->mqtt_username;
        }
        if (strlen(ctx->mqtt_password) > 0) {
            mqtt_cfg.credentials.authentication.password = ctx->mqtt_password;
        }

        ctx->mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
        esp_mqtt_client_register_event(ctx->mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(ctx->mqtt_client);

        bits = xEventGroupWaitBits(ctx->event_group,
                                   MQTT_CONNECTED_BIT,
                                   pdFALSE,
                                   pdFALSE,
                                   pdMS_TO_TICKS(10000));

        if (!(bits & MQTT_CONNECTED_BIT)) {
            ESP_LOGW(TAG, "MQTT connection timed out, but WiFi connected");
        }
    }

    return true;
}

void network_disconnect(void) {
    if (!ctx) {
        return;
    }

    if (ctx->mqtt_client) {
        esp_mqtt_client_stop(ctx->mqtt_client);
        esp_mqtt_client_destroy(ctx->mqtt_client);
        ctx->mqtt_client = NULL;
    }

    esp_wifi_disconnect();
    esp_wifi_stop();

    xEventGroupClearBits(ctx->event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | MQTT_CONNECTED_BIT);
    update_state(NETWORK_STATE_DISCONNECTED);
}

bool network_is_connected(void) {
    if (!ctx) {
        return false;
    }
    EventBits_t bits = xEventGroupGetBits(ctx->event_group);
    return (bits & MQTT_CONNECTED_BIT) != 0;
}

NetworkState network_get_state(void) {
    if (!ctx) {
        return NETWORK_STATE_DISCONNECTED;
    }
    return ctx->state;
}

bool network_mqtt_publish(const char* topic, const char* payload, size_t length) {
    if (!ctx || !ctx->mqtt_client) {
        return false;
    }
    int msg_id = esp_mqtt_client_publish(ctx->mqtt_client, topic, payload, length, 0, 0);
    return msg_id != -1;
}

bool network_mqtt_subscribe(const char* topic) {
    if (!ctx || !ctx->mqtt_client) {
        return false;
    }
    int msg_id = esp_mqtt_client_subscribe(ctx->mqtt_client, topic, 0);
    return msg_id != -1;
}

bool network_mqtt_unsubscribe(const char* topic) {
    if (!ctx || !ctx->mqtt_client) {
        return false;
    }
    int msg_id = esp_mqtt_client_unsubscribe(ctx->mqtt_client, topic);
    return msg_id != -1;
}

void network_set_message_callback(network_message_callback_t callback) {
    if (ctx) {
        ctx->message_callback = callback;
    }
}

void network_set_state_callback(network_state_callback_t callback) {
    if (ctx) {
        ctx->state_callback = callback;
    }
}

const char* network_get_ip_address(void) {
    if (!ctx) {
        return NULL;
    }
    return ctx->ip_address;
}
