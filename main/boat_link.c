#include "boat_link.h"

#include <stdlib.h>
#include <string.h>

#include "boat_protocol.h"
#include "boat_storage.h"
#include "boat_udp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char* TAG = "boat_link";

#define LINK_TASK_MS         50
#define CONNECT_TIMEOUT_MS   12000
#define RETRY_COUNT_MAX      3
#define RETRY_DELAY_MS       1000
#define MANUAL_DISCONNECT_MS 300
#define SCAN_RETRY_MS        200
#define SCAN_RETRY_MAX       5

typedef struct {
    SemaphoreHandle_t lock;
    bool initialized;

    BoatLinkState state;
    BoatScanState scan_state;
    BoatFailReason fail_reason;
    char target_ssid[33];
    char last_ssid[33];
    char saved_ssid[33];

    esp_ip4_addr_t gateway;
    boat_udp_t udp;
    uint8_t left;
    uint8_t right;
    uint8_t flags;

    BoatApInfo aps[BOAT_LINK_MAX_APS];
    int ap_count;
    bool scan_pending;
    bool scan_retry_pending;
    TickType_t scan_retry_at;
    int scan_retry_count;

    bool save_ssid_pending;
    char save_ssid[33];
    bool open_socket_pending;

    bool retry_pending;
    TickType_t retry_at;
    int retry_count;
    TickType_t state_since;
    TickType_t manual_disconnect_tick;
    bool user_cancelled;
    bool suppress_reconnect;
} boat_link_ctx_t;

static boat_link_ctx_t s_ctx;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;

static void link_lock(void)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
}

static void link_unlock(void)
{
    xSemaphoreGive(s_ctx.lock);
}

static void link_close_socket_locked(void)
{
    boat_udp_close(&s_ctx.udp);
}

static bool link_open_socket_locked(void)
{
    return boat_udp_open(&s_ctx.udp);
}

static void link_send_packet_locked(void)
{
    if (s_ctx.state != LINK_CONNECTED || s_ctx.udp.sock < 0) {
        return;
    }

    esp_ip4_addr_t target = s_ctx.gateway;
    if (target.addr == 0) {
        if (esp_netif_str_to_ip4(BOAT_UDP_TARGET_IP, &target) != ESP_OK) {
            return;
        }
    }

    boat_udp_send(&s_ctx.udp, target, s_ctx.left, s_ctx.right, s_ctx.flags);
}

static esp_err_t link_scan_start_driver(void)
{
    wifi_scan_config_t scan_cfg = {0};
    scan_cfg.show_hidden = false;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 100;
    scan_cfg.scan_time.active.max = 300;
    return esp_wifi_scan_start(&scan_cfg, false);
}

static BoatFailReason link_map_reason(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return BOAT_FAIL_PASSWORD;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return BOAT_FAIL_NOT_FOUND;
    default:
        return BOAT_FAIL_OTHER;
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        link_lock();
        if (s_ctx.scan_state == SCAN_RUNNING) {
            s_ctx.scan_pending = true;
        }
        link_unlock();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*)data;
        TickType_t now = xTaskGetTickCount();

        link_lock();
        link_close_socket_locked();

        BoatLinkState previous = s_ctx.state;
        if (s_ctx.user_cancelled) {
            s_ctx.state = LINK_DISCONNECTED;
            s_ctx.retry_pending = false;
        } else if (previous == LINK_CONNECTING &&
                   (now - s_ctx.manual_disconnect_tick) < pdMS_TO_TICKS(MANUAL_DISCONNECT_MS)) {
            /* Disconnect triggered by our own reconnect sequence. */
        } else if (previous == LINK_CONNECTED) {
            if (s_ctx.suppress_reconnect) {
                s_ctx.state = LINK_DISCONNECTED;
            } else {
                ESP_LOGW(TAG, "Connection lost, retrying");
                s_ctx.state = LINK_CONNECTING;
                s_ctx.state_since = now;
                s_ctx.retry_count = 0;
                s_ctx.retry_pending = true;
                s_ctx.retry_at = now + pdMS_TO_TICKS(RETRY_DELAY_MS);
                s_ctx.fail_reason = BOAT_FAIL_NONE;
            }
        } else if (previous == LINK_CONNECTING) {
            BoatFailReason reason = link_map_reason(event->reason);
            if (reason == BOAT_FAIL_PASSWORD) {
                ESP_LOGW(TAG, "Authentication failed");
                s_ctx.state = LINK_CONNECT_FAILED;
                s_ctx.fail_reason = reason;
                s_ctx.retry_pending = false;
            } else if (s_ctx.retry_count < RETRY_COUNT_MAX) {
                s_ctx.retry_count++;
                ESP_LOGW(TAG, "Connect failed (reason %d), retry %d/%d",
                         event->reason, s_ctx.retry_count, RETRY_COUNT_MAX);
                s_ctx.retry_pending = true;
                s_ctx.retry_at = now + pdMS_TO_TICKS(RETRY_DELAY_MS);
            } else {
                ESP_LOGE(TAG, "Connect failed (reason %d)", event->reason);
                s_ctx.state = LINK_CONNECT_FAILED;
                s_ctx.fail_reason = reason;
            }
        } else if (previous == LINK_CONNECT_FAILED) {
            /* Keep the failure reason for the UI. */
        } else {
            s_ctx.state = LINK_DISCONNECTED;
        }
        link_unlock();
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)data;

        link_lock();
        s_ctx.gateway = event->ip_info.gw;
        s_ctx.state = LINK_CONNECTED;
        s_ctx.fail_reason = BOAT_FAIL_NONE;
        s_ctx.retry_pending = false;
        s_ctx.retry_count = 0;
        s_ctx.user_cancelled = false;
        s_ctx.suppress_reconnect = false;
        s_ctx.open_socket_pending = true;
        if (s_ctx.target_ssid[0] != '\0') {
            strncpy(s_ctx.last_ssid, s_ctx.target_ssid, sizeof(s_ctx.last_ssid) - 1);
            s_ctx.last_ssid[sizeof(s_ctx.last_ssid) - 1] = '\0';
            if (strcmp(s_ctx.target_ssid, s_ctx.saved_ssid) != 0) {
                strncpy(s_ctx.save_ssid, s_ctx.target_ssid, sizeof(s_ctx.save_ssid) - 1);
                s_ctx.save_ssid[sizeof(s_ctx.save_ssid) - 1] = '\0';
                s_ctx.save_ssid_pending = true;
            }
        }
        link_unlock();

        ESP_LOGI(TAG, "Connected, IP " IPSTR ", gateway " IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
        return;
    }
}

static void link_process_scan_done(void)
{
    uint16_t num = 0;
    if (esp_wifi_scan_get_ap_num(&num) != ESP_OK) {
        num = 0;
    }

    wifi_ap_record_t* records = NULL;
    uint16_t fetched = 0;
    if (num > 0) {
        records = malloc((size_t)num * sizeof(wifi_ap_record_t));
    }

    if (records != NULL) {
        fetched = num;
        if (esp_wifi_scan_get_ap_records(&fetched, records) != ESP_OK) {
            fetched = 0;
        }
        if (fetched < num) {
            esp_wifi_clear_ap_list();
        }
    } else if (num > 0) {
        ESP_LOGW(TAG, "No memory for %u scan records", num);
        esp_wifi_clear_ap_list();
    }

    BoatApInfo list[BOAT_LINK_MAX_APS];
    int count = 0;
    const size_t prefix_len = strlen(BOAT_SSID_PREFIX);

    for (uint16_t i = 0; i < fetched; i++) {
        char ssid[33];
        memcpy(ssid, records[i].ssid, 32);
        ssid[32] = '\0';

        if (strncmp(ssid, BOAT_SSID_PREFIX, prefix_len) != 0) {
            continue;
        }

        int found = -1;
        for (int j = 0; j < count; j++) {
            if (strcmp(list[j].ssid, ssid) == 0) {
                found = j;
                break;
            }
        }

        if (found >= 0) {
            if (records[i].rssi > list[found].rssi) {
                list[found].rssi = records[i].rssi;
            }
        } else if (count < BOAT_LINK_MAX_APS) {
            strncpy(list[count].ssid, ssid, sizeof(list[count].ssid) - 1);
            list[count].ssid[sizeof(list[count].ssid) - 1] = '\0';
            list[count].rssi = records[i].rssi;
            count++;
        }
    }

    free(records);

    /* Strongest first. */
    for (int i = 1; i < count; i++) {
        BoatApInfo key = list[i];
        int j = i - 1;
        while (j >= 0 && list[j].rssi < key.rssi) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = key;
    }

    link_lock();
    if (count > 0) {
        memcpy(s_ctx.aps, list, (size_t)count * sizeof(BoatApInfo));
    }
    s_ctx.ap_count = count;
    s_ctx.scan_state = SCAN_DONE;
    s_ctx.scan_pending = false;
    s_ctx.scan_retry_pending = false;
    s_ctx.scan_retry_count = 0;
    link_unlock();

    ESP_LOGI(TAG, "Scan done, %d XC_BOT AP(s)", count);
}

static void link_load_saved_ssid(void)
{
    s_ctx.last_ssid[0] = '\0';
    s_ctx.saved_ssid[0] = '\0';

    if (boat_storage_load_ssid(s_ctx.last_ssid, sizeof(s_ctx.last_ssid))) {
        strncpy(s_ctx.saved_ssid, s_ctx.last_ssid, sizeof(s_ctx.saved_ssid) - 1);
        s_ctx.saved_ssid[sizeof(s_ctx.saved_ssid) - 1] = '\0';
    }
}

static void link_task(void* arg)
{
    (void)arg;
    TickType_t last_wakeup = xTaskGetTickCount();

    ESP_LOGI(TAG, "Link task started");

    while (1) {
        TickType_t now = xTaskGetTickCount();
        bool do_scan;
        bool do_scan_retry;
        bool do_save;
        bool do_retry;
        bool do_open;
        char save_ssid[33] = {0};
        BoatLinkState state;
        TickType_t state_since;

        link_lock();
        do_scan = s_ctx.scan_pending;
        do_scan_retry = s_ctx.scan_retry_pending &&
                        !s_ctx.scan_pending &&
                        s_ctx.scan_state == SCAN_RUNNING &&
                        ((int32_t)(now - s_ctx.scan_retry_at) >= 0);
        if (do_scan_retry) {
            s_ctx.scan_retry_pending = false;
        }
        do_save = s_ctx.save_ssid_pending;
        if (do_save) {
            memcpy(save_ssid, s_ctx.save_ssid, sizeof(save_ssid));
            s_ctx.save_ssid_pending = false;
        }
        do_open = s_ctx.open_socket_pending;
        s_ctx.open_socket_pending = false;
        do_retry = s_ctx.retry_pending &&
                   ((int32_t)(now - s_ctx.retry_at) >= 0);
        if (do_retry) {
            s_ctx.retry_pending = false;
            s_ctx.retry_at = now;
        }
        state = s_ctx.state;
        state_since = s_ctx.state_since;
        if (state == LINK_CONNECTED && s_ctx.udp.sock < 0) {
            do_open = true;
        }
        link_unlock();

        if (do_scan) {
            link_process_scan_done();
        }
        if (do_scan_retry) {
            esp_err_t err = link_scan_start_driver();
            link_lock();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Scan retry failed: %s", esp_err_to_name(err));
                if (s_ctx.scan_retry_count + 1 < SCAN_RETRY_MAX) {
                    s_ctx.scan_retry_count++;
                    s_ctx.scan_retry_pending = true;
                    s_ctx.scan_retry_at = now + pdMS_TO_TICKS(SCAN_RETRY_MS);
                } else {
                    s_ctx.scan_state = SCAN_DONE;
                    s_ctx.ap_count = 0;
                    s_ctx.scan_retry_count = 0;
                }
            } else {
                s_ctx.scan_retry_count = 0;
            }
            link_unlock();
        }
        if (do_open) {
            link_lock();
            link_open_socket_locked();
            link_unlock();
        }
        if (do_save) {
            if (boat_storage_save_ssid(save_ssid)) {
                link_lock();
                strncpy(s_ctx.saved_ssid, save_ssid, sizeof(s_ctx.saved_ssid) - 1);
                s_ctx.saved_ssid[sizeof(s_ctx.saved_ssid) - 1] = '\0';
                link_unlock();
            }
        }
        if (do_retry) {
            link_lock();
            bool allowed = (s_ctx.state == LINK_CONNECTING &&
                            !s_ctx.user_cancelled &&
                            !s_ctx.suppress_reconnect);
            link_unlock();
            if (allowed) {
                ESP_LOGI(TAG, "Retrying connection");
                esp_wifi_connect();
            }
        }

        if (state == LINK_CONNECTING &&
            (now - state_since) >= pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)) {
            link_lock();
            if (s_ctx.state == LINK_CONNECTING) {
                ESP_LOGW(TAG, "Connect timeout");
                s_ctx.state = LINK_CONNECT_FAILED;
                s_ctx.fail_reason = BOAT_FAIL_TIMEOUT;
                s_ctx.retry_pending = false;
            }
            link_unlock();
        }

        link_lock();
        link_send_packet_locked();
        link_unlock();

        vTaskDelayUntil(&last_wakeup, pdMS_TO_TICKS(LINK_TASK_MS));
    }
}

bool boat_link_init(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    boat_udp_init(&s_ctx.udp);
    s_ctx.lock = xSemaphoreCreateMutex();
    if (s_ctx.lock == NULL) {
        ESP_LOGE(TAG, "Cannot create mutex");
        return false;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return false;
    }

    link_load_saved_ssid();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop init failed: %s", esp_err_to_name(loop_err));
        return false;
    }

    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "Cannot create STA netif");
        return false;
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wifi_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed");
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &s_wifi_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &s_ip_handler));

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot set STA mode");
        return false;
    }
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start WiFi");
        return false;
    }

    esp_wifi_set_country_code("CN", true);
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_ctx.state = LINK_DISCONNECTED;
    s_ctx.scan_state = SCAN_IDLE;
    s_ctx.initialized = true;

    if (xTaskCreate(link_task, "boat_link", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create link task");
        s_ctx.initialized = false;
        return false;
    }

    if (s_ctx.last_ssid[0] != '\0') {
        ESP_LOGI(TAG, "Auto-connecting to last boat: %s", s_ctx.last_ssid);
        boat_link_connect(s_ctx.last_ssid);
    }

    return true;
}

void boat_link_start_scan(void)
{
    if (!s_ctx.initialized) {
        return;
    }

    bool need_disconnect = false;
    link_lock();
    if (s_ctx.state == LINK_CONNECTING) {
        s_ctx.user_cancelled = true;
        s_ctx.state = LINK_DISCONNECTED;
        s_ctx.retry_pending = false;
        link_close_socket_locked();
        s_ctx.manual_disconnect_tick = xTaskGetTickCount();
        need_disconnect = true;
    }
    s_ctx.suppress_reconnect = true;
    s_ctx.scan_state = SCAN_RUNNING;
    s_ctx.scan_pending = false;
    s_ctx.scan_retry_pending = false;
    s_ctx.scan_retry_count = 0;
    s_ctx.ap_count = 0;
    link_unlock();

    if (need_disconnect) {
        esp_wifi_disconnect();
    }

    esp_wifi_scan_stop();
    esp_err_t err = link_scan_start_driver();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scan start failed: %s, will retry", esp_err_to_name(err));
        link_lock();
        s_ctx.scan_retry_pending = true;
        s_ctx.scan_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(SCAN_RETRY_MS);
        link_unlock();
    }
}

void boat_link_stop_scan(void)
{
    if (!s_ctx.initialized) {
        return;
    }

    esp_wifi_scan_stop();

    char reconnect_ssid[33] = {0};
    bool reconnect = false;

    link_lock();
    s_ctx.scan_state = SCAN_IDLE;
    s_ctx.suppress_reconnect = false;
    s_ctx.scan_retry_pending = false;
    s_ctx.scan_retry_count = 0;
    if (s_ctx.state != LINK_CONNECTED && s_ctx.state != LINK_CONNECTING &&
        s_ctx.last_ssid[0] != '\0') {
        strncpy(reconnect_ssid, s_ctx.last_ssid, sizeof(reconnect_ssid) - 1);
        reconnect = true;
    }
    link_unlock();

    if (reconnect) {
        boat_link_connect(reconnect_ssid);
    }
}

BoatScanState boat_link_get_scan_state(void)
{
    if (s_ctx.lock == NULL) {
        return SCAN_IDLE;
    }
    link_lock();
    BoatScanState state = s_ctx.scan_state;
    link_unlock();
    return state;
}

int boat_link_copy_aps(BoatApInfo* out, int max)
{
    if (out == NULL || max <= 0 || s_ctx.lock == NULL) {
        return 0;
    }

    link_lock();
    int count = s_ctx.ap_count;
    if (count > max) {
        count = max;
    }
    memcpy(out, s_ctx.aps, (size_t)count * sizeof(BoatApInfo));
    link_unlock();
    return count;
}

void boat_link_connect(const char* ssid)
{
    if (!s_ctx.initialized || ssid == NULL || ssid[0] == '\0') {
        return;
    }

    wifi_config_t wifi_cfg = {0};
    strncpy((char*)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char*)wifi_cfg.sta.password, BOAT_AP_PASSWORD,
            sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    link_lock();
    strncpy(s_ctx.target_ssid, ssid, sizeof(s_ctx.target_ssid) - 1);
    s_ctx.target_ssid[sizeof(s_ctx.target_ssid) - 1] = '\0';
    s_ctx.user_cancelled = false;
    s_ctx.suppress_reconnect = false;
    s_ctx.retry_pending = false;
    s_ctx.retry_count = 0;
    s_ctx.fail_reason = BOAT_FAIL_NONE;
    s_ctx.state = LINK_CONNECTING;
    s_ctx.state_since = xTaskGetTickCount();
    s_ctx.manual_disconnect_tick = s_ctx.state_since;
    s_ctx.scan_state = SCAN_IDLE;
    s_ctx.scan_pending = false;
    s_ctx.scan_retry_pending = false;
    s_ctx.scan_retry_count = 0;
    link_close_socket_locked();
    link_unlock();

    ESP_LOGI(TAG, "Connecting to %s", ssid);

    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set config failed: %s", esp_err_to_name(err));
        link_lock();
        s_ctx.state = LINK_CONNECT_FAILED;
        s_ctx.fail_reason = BOAT_FAIL_OTHER;
        link_unlock();
        return;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Connect call failed: %s", esp_err_to_name(err));
        link_lock();
        s_ctx.state = LINK_CONNECT_FAILED;
        s_ctx.fail_reason = BOAT_FAIL_OTHER;
        link_unlock();
    }
}

void boat_link_cancel_connect(void)
{
    if (!s_ctx.initialized) {
        return;
    }

    link_lock();
    s_ctx.user_cancelled = true;
    s_ctx.retry_pending = false;
    s_ctx.state = LINK_DISCONNECTED;
    s_ctx.manual_disconnect_tick = xTaskGetTickCount();
    link_close_socket_locked();
    link_unlock();

    esp_wifi_disconnect();
}

BoatLinkState boat_link_get_state(char* ssid_out, size_t ssid_len)
{
    if (s_ctx.lock == NULL) {
        if (ssid_out != NULL && ssid_len > 0) {
            ssid_out[0] = '\0';
        }
        return LINK_DISCONNECTED;
    }

    link_lock();
    BoatLinkState state = s_ctx.state;
    if (ssid_out != NULL && ssid_len > 0) {
        strncpy(ssid_out, s_ctx.target_ssid, ssid_len - 1);
        ssid_out[ssid_len - 1] = '\0';
    }
    link_unlock();
    return state;
}

BoatFailReason boat_link_get_fail_reason(void)
{
    if (s_ctx.lock == NULL) {
        return BOAT_FAIL_NONE;
    }
    link_lock();
    BoatFailReason reason = s_ctx.fail_reason;
    link_unlock();
    return reason;
}

void boat_link_set_command(uint8_t left, uint8_t right, bool brake)
{
    if (s_ctx.lock == NULL) {
        return;
    }

    if (left > BOAT_THROTTLE_MAX) {
        left = BOAT_THROTTLE_MAX;
    }
    if (right > BOAT_THROTTLE_MAX) {
        right = BOAT_THROTTLE_MAX;
    }

    link_lock();
    s_ctx.left = left;
    s_ctx.right = right;
    if (brake) {
        s_ctx.flags |= BOAT_FLAG_BRAKE;
    } else {
        s_ctx.flags &= (uint8_t)~BOAT_FLAG_BRAKE;
    }
    link_unlock();
}

void boat_link_set_outputs(bool light, bool buzzer)
{
    if (s_ctx.lock == NULL) {
        return;
    }

    link_lock();
    if (light) {
        s_ctx.flags |= BOAT_FLAG_LIGHT;
    } else {
        s_ctx.flags &= (uint8_t)~BOAT_FLAG_LIGHT;
    }
    if (buzzer) {
        s_ctx.flags |= BOAT_FLAG_BUZZER;
    } else {
        s_ctx.flags &= (uint8_t)~BOAT_FLAG_BUZZER;
    }
    link_unlock();
}

void boat_link_stop_now(void)
{
    if (s_ctx.lock == NULL) {
        return;
    }

    link_lock();
    s_ctx.left = 0;
    s_ctx.right = 0;
    s_ctx.flags &= (uint8_t)~BOAT_FLAG_BRAKE;
    link_send_packet_locked();
    link_unlock();
}
