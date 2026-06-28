#include "chess_bluetooth.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"
#include <string.h>
#include <stdlib.h>

#define BT_CHESS_TAG "CHESS_BT"
#define BT_CHESS_SERVER_NAME "XC_CHESS_SERVER"
#define MAX_FOUND_DEVICES 5
#define SPP_BUFFER_SIZE 256

static bool s_initialized = false;
static BluetoothChessState s_state = BT_CHESS_STATE_IDLE;
static BluetoothChessRole s_role = BT_CHESS_ROLE_NONE;
static ChessColor s_local_color = COLOR_WHITE;
static bool s_is_my_turn = true;
static uint32_t s_spp_handle = 0;
static uint32_t s_spp_server_handle = 0;
static uint8_t s_target_bda[6] = {0};

static FoundBluetoothDevice s_found_devices[MAX_FOUND_DEVICES];
static int s_found_device_count = 0;

static bt_chess_state_callback_t s_state_callback = NULL;
static bt_chess_device_found_callback_t s_device_found_callback = NULL;
static bt_chess_device_list_complete_callback_t s_device_list_complete_callback = NULL;
static bt_chess_connected_callback_t s_connected_callback = NULL;
static bt_chess_disconnected_callback_t s_disconnected_callback = NULL;
static bt_chess_invite_received_callback_t s_invite_callback = NULL;
static bt_chess_invite_accepted_callback_t s_invite_accepted_callback = NULL;
static bt_chess_move_received_callback_t s_move_callback = NULL;
static bt_chess_game_over_callback_t s_game_over_callback = NULL;
static bt_chess_chat_received_callback_t s_chat_callback = NULL;

static uint8_t s_rx_buffer[SPP_BUFFER_SIZE];
static int s_rx_buffer_len = 0;

static char* bda2str(const uint8_t* bda, char* str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return str;
}

static void generate_hash_from_bda(const uint8_t* bda, char* hash, size_t hash_size)
{
    if (bda == NULL || hash == NULL || hash_size < 7) {
        if (hash != NULL && hash_size >= 7) {
            strcpy(hash, "000000");
        }
        return;
    }
    snprintf(hash, hash_size, "%02x%02x%02x", bda[3], bda[4], bda[5]);
    for (int i = 0; i < 6; i++) {
        if (hash[i] >= 'a' && hash[i] <= 'z') {
            hash[i] -= ('a' - 'A');
        }
    }
}

static void generate_device_name(char* name, size_t name_size)
{
    const uint8_t* bda = esp_bt_dev_get_address();
    char hash[7];
    if (bda == NULL) {
        snprintf(name, name_size, "%s000000", BT_CHESS_PREFIX);
        return;
    }
    generate_hash_from_bda(bda, hash, sizeof(hash));
    snprintf(name, name_size, "%s%s", BT_CHESS_PREFIX, hash);
}

static bool is_chess_device(const char* name, char* hash_out, size_t hash_size)
{
    if (name == NULL) return false;
    size_t prefix_len = strlen(BT_CHESS_PREFIX);
    if (strlen(name) < prefix_len + 6) return false;
    if (strncmp(name, BT_CHESS_PREFIX, prefix_len) != 0) return false;
    if (hash_out != NULL && hash_size >= 7) {
        strncpy(hash_out, name + prefix_len, 6);
        hash_out[6] = '\0';
    }
    return true;
}

static bool get_name_from_eir(uint8_t* eir, char* bdname, uint8_t* bdname_len)
{
    uint8_t* rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;
    if (!eir) return false;
    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }
    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }
        if (bdname) {
            memcpy(bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (bdname_len) {
            *bdname_len = rmt_bdname_len;
        }
        return true;
    }
    return false;
}

static void set_state(BluetoothChessState new_state)
{
    if (s_state != new_state) {
        ESP_LOGI(BT_CHESS_TAG, "State changed: %d -> %d", s_state, new_state);
        s_state = new_state;
        if (s_state_callback) {
            s_state_callback(new_state);
        }
    }
}

static bool send_data(const uint8_t* data, size_t len)
{
    if (s_spp_handle == 0) {
        ESP_LOGE(BT_CHESS_TAG, "Not connected, cannot send data");
        return false;
    }
    esp_err_t ret = esp_spp_write(s_spp_handle, len, (uint8_t*)data);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_CHESS_TAG, "Failed to send data: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

static void parse_message(const uint8_t* data, size_t len)
{
    if (len < 1) return;
    uint8_t msg_type = data[0];
    const uint8_t* payload = data + 1;
    size_t payload_len = len - 1;
    switch (msg_type) {
        case BT_CHESS_MSG_HELLO:
            ESP_LOGI(BT_CHESS_TAG, "Received HELLO message");
            if (s_role == BT_CHESS_ROLE_HOST && payload_len >= 1) {
                s_local_color = payload[0];
                s_is_my_turn = (s_local_color == COLOR_WHITE);
                set_state(BT_CHESS_STATE_GAME_READY);
                if (s_invite_accepted_callback) {
                    s_invite_accepted_callback(s_local_color);
                }
            }
            break;
        case BT_CHESS_MSG_INVITE:
            ESP_LOGI(BT_CHESS_TAG, "Received INVITE message");
            if (s_invite_callback) {
                s_invite_callback();
            }
            break;
        case BT_CHESS_MSG_ACCEPT:
            ESP_LOGI(BT_CHESS_TAG, "Received ACCEPT message");
            if (s_role == BT_CHESS_ROLE_GUEST && payload_len >= 1) {
                s_local_color = payload[0];
                s_is_my_turn = (s_local_color == COLOR_WHITE);
                set_state(BT_CHESS_STATE_GAME_READY);
                if (s_invite_accepted_callback) {
                    s_invite_accepted_callback(s_local_color);
                }
            }
            break;
        case BT_CHESS_MSG_REJECT:
            ESP_LOGI(BT_CHESS_TAG, "Received REJECT message");
            set_state(BT_CHESS_STATE_IDLE);
            break;
        case BT_CHESS_MSG_MOVE:
            ESP_LOGI(BT_CHESS_TAG, "Received MOVE message");
            if (payload_len >= 5) {
                int from_x = payload[0];
                int from_y = payload[1];
                int to_x = payload[2];
                int to_y = payload[3];
                ChessPieceType promotion = payload[4];
                s_is_my_turn = true;
                if (s_move_callback) {
                    s_move_callback(from_x, from_y, to_x, to_y, promotion);
                }
            }
            break;
        case BT_CHESS_MSG_SYNC:
            ESP_LOGI(BT_CHESS_TAG, "Received SYNC message");
            break;
        case BT_CHESS_MSG_GAME_OVER:
            ESP_LOGI(BT_CHESS_TAG, "Received GAME_OVER message");
            if (payload_len >= 1) {
                ChessColor winner = payload[0];
                set_state(BT_CHESS_STATE_GAME_OVER);
                if (s_game_over_callback) {
                    s_game_over_callback(winner);
                }
            }
            break;
        case BT_CHESS_MSG_RESIGN:
            ESP_LOGI(BT_CHESS_TAG, "Received RESIGN message");
            ChessColor resigner_color = (s_role == BT_CHESS_ROLE_HOST) ? COLOR_BLACK : COLOR_WHITE;
            ChessColor winner = (resigner_color == COLOR_WHITE) ? COLOR_BLACK : COLOR_WHITE;
            set_state(BT_CHESS_STATE_GAME_OVER);
            if (s_game_over_callback) {
                s_game_over_callback(winner);
            }
            break;
        case BT_CHESS_MSG_CHAT:
            ESP_LOGI(BT_CHESS_TAG, "Received CHAT message");
            if (payload_len > 0 && s_chat_callback) {
                char chat_msg[256];
                size_t copy_len = (payload_len < 255) ? payload_len : 255;
                memcpy(chat_msg, payload, copy_len);
                chat_msg[copy_len] = '\0';
                s_chat_callback(chat_msg);
            }
            break;
        default:
            ESP_LOGW(BT_CHESS_TAG, "Unknown message type: %d", msg_type);
            break;
    }
}

static void process_rx_buffer(void)
{
    if (s_rx_buffer_len < 2) return;
    uint8_t msg_len = s_rx_buffer[0];
    if (s_rx_buffer_len < msg_len + 1) return;
    parse_message(&s_rx_buffer[1], msg_len);
    int remaining = s_rx_buffer_len - (msg_len + 1);
    if (remaining > 0) {
        memmove(s_rx_buffer, &s_rx_buffer[msg_len + 1], remaining);
    }
    s_rx_buffer_len = remaining;
    if (s_rx_buffer_len >= 2) {
        process_rx_buffer();
    }
}

static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t* param)
{
    char bda_str[18];
    switch (event) {
        case ESP_SPP_INIT_EVT:
            ESP_LOGI(BT_CHESS_TAG, "SPP initialized");
            break;
        case ESP_SPP_DISCOVERY_COMP_EVT:
            ESP_LOGI(BT_CHESS_TAG, "SPP discovery complete, status: %d, scn_num: %d",
                     param->disc_comp.status, param->disc_comp.scn_num);
            if (param->disc_comp.status == ESP_SPP_SUCCESS && param->disc_comp.scn_num > 0) {
                esp_spp_connect(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_MASTER,
                               param->disc_comp.scn[0], s_target_bda);
            } else {
                set_state(BT_CHESS_STATE_IDLE);
            }
            break;
        case ESP_SPP_OPEN_EVT:
        case ESP_SPP_SRV_OPEN_EVT:
            ESP_LOGI(BT_CHESS_TAG, "SPP %s, handle: %"PRIu32", BDA: %s",
                     (event == ESP_SPP_OPEN_EVT) ? "opened" : "server opened",
                     (event == ESP_SPP_OPEN_EVT) ? param->open.handle : param->srv_open.handle,
                     bda2str((event == ESP_SPP_OPEN_EVT) ? param->open.rem_bda : param->srv_open.rem_bda,
                            bda_str, sizeof(bda_str)));
            s_spp_handle = (event == ESP_SPP_OPEN_EVT) ? param->open.handle : param->srv_open.handle;
            set_state(BT_CHESS_STATE_CONNECTED);
            if (s_connected_callback) {
                s_connected_callback();
            }
            if (s_role == BT_CHESS_ROLE_HOST) {
                s_local_color = COLOR_WHITE;
                s_is_my_turn = true;
                uint8_t msg[3];
                msg[0] = 2;
                msg[1] = BT_CHESS_MSG_HELLO;
                msg[2] = COLOR_BLACK;
                send_data(msg, 3);
                if (s_invite_callback) {
                    s_invite_callback();
                }
            }
            break;
        case ESP_SPP_CLOSE_EVT:
            ESP_LOGI(BT_CHESS_TAG, "SPP closed, handle: %"PRIu32, param->close.handle);
            if (s_spp_handle == param->close.handle) {
                s_spp_handle = 0;
            }
            s_rx_buffer_len = 0;
            set_state(BT_CHESS_STATE_IDLE);
            if (s_disconnected_callback) {
                s_disconnected_callback();
            }
            break;
        case ESP_SPP_START_EVT:
            ESP_LOGI(BT_CHESS_TAG, "SPP server started, handle: %"PRIu32, param->start.handle);
            s_spp_server_handle = param->start.handle;
            char device_name[BT_CHESS_NAME_LEN];
            generate_device_name(device_name, sizeof(device_name));
            esp_bt_gap_set_device_name(device_name);
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            set_state(BT_CHESS_STATE_ADVERTISING);
            break;
        case ESP_SPP_DATA_IND_EVT:
            ESP_LOGI(BT_CHESS_TAG, "SPP data received, len: %d", param->data_ind.len);
            int remaining = SPP_BUFFER_SIZE - s_rx_buffer_len;
            int to_copy = (param->data_ind.len < remaining) ? param->data_ind.len : remaining;
            if (to_copy > 0) {
                memcpy(&s_rx_buffer[s_rx_buffer_len], param->data_ind.data, to_copy);
                s_rx_buffer_len += to_copy;
                process_rx_buffer();
            }
            break;
        case ESP_SPP_WRITE_EVT:
            break;
        case ESP_SPP_CONG_EVT:
            ESP_LOGI(BT_CHESS_TAG, "SPP congestion: %d", param->cong.cong);
            break;
        default:
            break;
    }
}

static void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param)
{
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT:
            ESP_LOGI(BT_CHESS_TAG, "Discovery result");
            char bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
            uint8_t bdname_len = 0;
            for (int i = 0; i < param->disc_res.num_prop; i++) {
                if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_EIR &&
                    get_name_from_eir(param->disc_res.prop[i].val, bdname, &bdname_len)) {
                    char hash[7];
                    if (is_chess_device(bdname, hash, sizeof(hash))) {
                        ESP_LOGI(BT_CHESS_TAG, "Found chess device: %s (hash: %s)", bdname, hash);
                        bool already_found = false;
                        for (int j = 0; j < s_found_device_count; j++) {
                            if (memcmp(s_found_devices[j].bda, param->disc_res.bda, 6) == 0) {
                                already_found = true;
                                break;
                            }
                        }
                        if (!already_found && s_found_device_count < MAX_FOUND_DEVICES) {
                            FoundBluetoothDevice* dev = &s_found_devices[s_found_device_count];
                            memcpy(dev->bda, param->disc_res.bda, 6);
                            strncpy(dev->name, bdname, BT_CHESS_NAME_LEN - 1);
                            dev->name[BT_CHESS_NAME_LEN - 1] = '\0';
                            strncpy(dev->hash, hash, 6);
                            dev->hash[6] = '\0';
                            if (s_device_found_callback) {
                                s_device_found_callback(dev, s_found_device_count);
                            }
                            s_found_device_count++;
                        }
                    }
                }
            }
            break;
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            ESP_LOGI(BT_CHESS_TAG, "Discovery state changed: %d", param->disc_st_chg.state);
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED && s_state == BT_CHESS_STATE_SCANNING) {
                if (s_device_list_complete_callback) {
                    s_device_list_complete_callback(s_found_device_count);
                }
            }
            break;
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(BT_CHESS_TAG, "Authentication successful: %s", param->auth_cmpl.device_name);
            } else {
                ESP_LOGE(BT_CHESS_TAG, "Authentication failed: %d", param->auth_cmpl.stat);
            }
            break;
        case ESP_BT_GAP_PIN_REQ_EVT:
            ESP_LOGI(BT_CHESS_TAG, "PIN request, min 16 digit: %d", param->pin_req.min_16_digit);
            esp_bt_pin_code_t pin_code = {'1','2','3','4'};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
            break;
        case ESP_BT_GAP_CFM_REQ_EVT:
            ESP_LOGI(BT_CHESS_TAG, "SSP confirm request: %06"PRIu32, param->cfm_req.num_val);
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;
        case ESP_BT_GAP_KEY_NOTIF_EVT:
            ESP_LOGI(BT_CHESS_TAG, "SSP key notification: %06"PRIu32, param->key_notif.passkey);
            break;
        case ESP_BT_GAP_KEY_REQ_EVT:
            ESP_LOGI(BT_CHESS_TAG, "SSP key request");
            break;
        default:
            ESP_LOGI(BT_CHESS_TAG, "GAP event: %d", event);
            break;
    }
}

void bt_chess_init(void)
{
    if (s_initialized) {
        ESP_LOGW(BT_CHESS_TAG, "Already initialized, skipping");
        return;
    }
    ESP_LOGI(BT_CHESS_TAG, "Initializing Bluetooth chess module");
    s_state = BT_CHESS_STATE_IDLE;
    s_role = BT_CHESS_ROLE_NONE;
    s_spp_handle = 0;
    s_spp_server_handle = 0;
    s_found_device_count = 0;
    s_rx_buffer_len = 0;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(BT_CHESS_TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(BT_CHESS_TAG, "controller init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(BT_CHESS_TAG, "controller enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(BT_CHESS_TAG, "bluedroid init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(BT_CHESS_TAG, "bluedroid enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_gap_register_callback(esp_bt_gap_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_CHESS_TAG, "GAP register failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_spp_register_callback(esp_spp_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_CHESS_TAG, "SPP register failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_spp_cfg_t bt_spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0,
    };
    ret = esp_spp_enhanced_init(&bt_spp_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(BT_CHESS_TAG, "SPP init failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_pin_code_t pin_code = {'1','2','3','4'};
    esp_bt_gap_set_pin(pin_type, 4, pin_code);

    char bda_str[18];
    char hash[7];
    const uint8_t* bda = esp_bt_dev_get_address();
    if (bda != NULL) {
        generate_hash_from_bda(bda, hash, sizeof(hash));
        ESP_LOGI(BT_CHESS_TAG, "Own BDA: %s, Hash: %s", bda2str(bda, bda_str, sizeof(bda_str)), hash);
    } else {
        ESP_LOGW(BT_CHESS_TAG, "BDA not available yet");
    }

    s_initialized = true;
}

void bt_chess_deinit(void)
{
    if (!s_initialized) {
        return;
    }
    ESP_LOGI(BT_CHESS_TAG, "Deinitializing Bluetooth chess module");
    if (s_spp_handle != 0) {
        esp_spp_disconnect(s_spp_handle);
        s_spp_handle = 0;
    }
    if (s_spp_server_handle != 0) {
        esp_spp_stop_srv();
        s_spp_server_handle = 0;
    }
    esp_spp_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    s_state = BT_CHESS_STATE_IDLE;
    s_role = BT_CHESS_ROLE_NONE;
    s_found_device_count = 0;
    s_initialized = false;
}

BluetoothChessState bt_chess_get_state(void)
{
    return s_state;
}

BluetoothChessRole bt_chess_get_role(void)
{
    return s_role;
}

ChessColor bt_chess_get_local_color(void)
{
    return s_local_color;
}

bool bt_chess_is_my_turn(void)
{
    return s_is_my_turn;
}

bool bt_chess_start_hosting(void)
{
    ESP_LOGI(BT_CHESS_TAG, "Starting hosting");
    if (!s_initialized) {
        ESP_LOGE(BT_CHESS_TAG, "Bluetooth not initialized");
        return false;
    }
    if (s_spp_server_handle != 0) {
        ESP_LOGW(BT_CHESS_TAG, "Server already running, stopping first");
        bt_chess_stop_hosting();
    }
    if (s_state != BT_CHESS_STATE_IDLE) {
        ESP_LOGW(BT_CHESS_TAG, "Cannot host in state: %d", s_state);
        return false;
    }
    s_role = BT_CHESS_ROLE_HOST;
    s_found_device_count = 0;
    esp_err_t ret = esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, BT_CHESS_SERVER_NAME);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_CHESS_TAG, "Failed to start SPP server: %s", esp_err_to_name(ret));
        s_role = BT_CHESS_ROLE_NONE;
        return false;
    }
    return true;
}

void bt_chess_stop_hosting(void)
{
    ESP_LOGI(BT_CHESS_TAG, "Stopping hosting");
    if (s_spp_server_handle != 0) {
        esp_spp_stop_srv();
        s_spp_server_handle = 0;
    }
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    if (s_state == BT_CHESS_STATE_ADVERTISING || s_role == BT_CHESS_ROLE_HOST) {
        set_state(BT_CHESS_STATE_IDLE);
        s_role = BT_CHESS_ROLE_NONE;
    }
}

bool bt_chess_accept_connection(void)
{
    ESP_LOGI(BT_CHESS_TAG, "Accepting connection");
    if (s_state != BT_CHESS_STATE_CONNECTED) {
        ESP_LOGW(BT_CHESS_TAG, "Cannot accept in state: %d", s_state);
        return false;
    }
    uint8_t msg[3];
    msg[0] = 2;
    msg[1] = BT_CHESS_MSG_ACCEPT;
    msg[2] = COLOR_WHITE;
    if (!send_data(msg, 3)) {
        return false;
    }
    s_local_color = COLOR_WHITE;
    s_is_my_turn = true;
    set_state(BT_CHESS_STATE_GAME_READY);
    return true;
}

void bt_chess_reject_connection(void)
{
    ESP_LOGI(BT_CHESS_TAG, "Rejecting connection");
    if (s_spp_handle != 0) {
        uint8_t msg[2];
        msg[0] = 1;
        msg[1] = BT_CHESS_MSG_REJECT;
        send_data(msg, 2);
        esp_spp_disconnect(s_spp_handle);
    }
    set_state(BT_CHESS_STATE_IDLE);
    s_role = BT_CHESS_ROLE_NONE;
}

bool bt_chess_start_scanning(void)
{
    ESP_LOGI(BT_CHESS_TAG, "Starting scan");
    if (!s_initialized) {
        ESP_LOGE(BT_CHESS_TAG, "Bluetooth not initialized");
        return false;
    }
    if (s_state != BT_CHESS_STATE_IDLE) {
        ESP_LOGW(BT_CHESS_TAG, "Cannot scan in state: %d", s_state);
        return false;
    }
    s_role = BT_CHESS_ROLE_GUEST;
    s_found_device_count = 0;
    char device_name[BT_CHESS_NAME_LEN];
    generate_device_name(device_name, sizeof(device_name));
    esp_bt_gap_set_device_name(device_name);
    set_state(BT_CHESS_STATE_SCANNING);
    esp_err_t ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 30, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_CHESS_TAG, "Failed to start discovery: %s", esp_err_to_name(ret));
        set_state(BT_CHESS_STATE_IDLE);
        s_role = BT_CHESS_ROLE_NONE;
        return false;
    }
    return true;
}

void bt_chess_stop_scanning(void)
{
    ESP_LOGI(BT_CHESS_TAG, "Stopping scan");
    if (s_state == BT_CHESS_STATE_SCANNING) {
        esp_bt_gap_cancel_discovery();
        set_state(BT_CHESS_STATE_IDLE);
        s_role = BT_CHESS_ROLE_NONE;
    }
}

int bt_chess_get_found_device_count(void)
{
    return s_found_device_count;
}

const FoundBluetoothDevice* bt_chess_get_found_device(int index)
{
    if (index < 0 || index >= s_found_device_count) {
        return NULL;
    }
    return &s_found_devices[index];
}

bool bt_chess_connect_to_device(int index)
{
    ESP_LOGI(BT_CHESS_TAG, "Connecting to device index: %d", index);
    if (!s_initialized) {
        ESP_LOGE(BT_CHESS_TAG, "Bluetooth not initialized");
        return false;
    }
    if (index < 0 || index >= s_found_device_count) {
        ESP_LOGE(BT_CHESS_TAG, "Invalid device index");
        return false;
    }
    if (s_state != BT_CHESS_STATE_SCANNING) {
        ESP_LOGW(BT_CHESS_TAG, "Cannot connect in state: %d", s_state);
        return false;
    }
    esp_bt_gap_cancel_discovery();
    memcpy(s_target_bda, s_found_devices[index].bda, 6);
    set_state(BT_CHESS_STATE_CONNECTING);
    esp_err_t ret = esp_spp_start_discovery(s_target_bda);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_CHESS_TAG, "Failed to start SPP discovery: %s", esp_err_to_name(ret));
        set_state(BT_CHESS_STATE_IDLE);
        return false;
    }
    return true;
}

bool bt_chess_connect_to_hash(const char* hash)
{
    ESP_LOGI(BT_CHESS_TAG, "Connecting to hash: %s", hash);
    if (hash == NULL || strlen(hash) != 6) {
        ESP_LOGE(BT_CHESS_TAG, "Invalid hash");
        return false;
    }
    for (int i = 0; i < s_found_device_count; i++) {
        if (strcasecmp(s_found_devices[i].hash, hash) == 0) {
            return bt_chess_connect_to_device(i);
        }
    }
    ESP_LOGW(BT_CHESS_TAG, "Device with hash %s not found", hash);
    return false;
}

bool bt_chess_send_move(int from_x, int from_y, int to_x, int to_y, ChessPieceType promotion)
{
    ESP_LOGI(BT_CHESS_TAG, "Sending move: %d,%d -> %d,%d (promo: %d)", from_x, from_y, to_x, to_y, promotion);
    if (s_spp_handle == 0) {
        ESP_LOGE(BT_CHESS_TAG, "Not connected");
        return false;
    }
    uint8_t msg[7];
    msg[0] = 6;
    msg[1] = BT_CHESS_MSG_MOVE;
    msg[2] = from_x & 0xFF;
    msg[3] = from_y & 0xFF;
    msg[4] = to_x & 0xFF;
    msg[5] = to_y & 0xFF;
    msg[6] = promotion & 0xFF;
    if (send_data(msg, 7)) {
        s_is_my_turn = false;
        if (s_state == BT_CHESS_STATE_GAME_READY) {
            set_state(BT_CHESS_STATE_PLAYING);
        }
        return true;
    }
    return false;
}

bool bt_chess_send_sync(const ChessGame* game)
{
    ESP_LOGI(BT_CHESS_TAG, "Sending game sync");
    if (s_spp_handle == 0 || game == NULL) {
        return false;
    }
    uint8_t msg[2];
    msg[0] = 1;
    msg[1] = BT_CHESS_MSG_SYNC;
    return send_data(msg, 2);
}

bool bt_chess_send_resign(void)
{
    ESP_LOGI(BT_CHESS_TAG, "Sending resignation");
    if (s_spp_handle == 0) {
        return false;
    }
    uint8_t msg[2];
    msg[0] = 1;
    msg[1] = BT_CHESS_MSG_RESIGN;
    if (send_data(msg, 2)) {
        ChessColor winner = (s_local_color == COLOR_WHITE) ? COLOR_BLACK : COLOR_WHITE;
        set_state(BT_CHESS_STATE_GAME_OVER);
        if (s_game_over_callback) {
            s_game_over_callback(winner);
        }
        return true;
    }
    return false;
}

bool bt_chess_send_chat(const char* message)
{
    if (s_spp_handle == 0 || message == NULL) {
        return false;
    }
    size_t msg_len = strlen(message);
    if (msg_len > 250) {
        msg_len = 250;
    }
    uint8_t msg[256];
    msg[0] = 1 + msg_len;
    msg[1] = BT_CHESS_MSG_CHAT;
    memcpy(&msg[2], message, msg_len);
    return send_data(msg, 2 + msg_len);
}

bool bt_chess_send_game_over(ChessColor winner)
{
    ESP_LOGI(BT_CHESS_TAG, "Sending game over, winner: %d", winner);
    if (s_spp_handle == 0) {
        return false;
    }
    uint8_t msg[3];
    msg[0] = 2;
    msg[1] = BT_CHESS_MSG_GAME_OVER;
    msg[2] = winner;
    if (send_data(msg, 3)) {
        set_state(BT_CHESS_STATE_GAME_OVER);
        return true;
    }
    return false;
}

void bt_chess_disconnect(void)
{
    ESP_LOGI(BT_CHESS_TAG, "Disconnecting");
    if (s_spp_handle != 0) {
        esp_spp_disconnect(s_spp_handle);
        s_spp_handle = 0;
    }
    if (s_spp_server_handle != 0) {
        esp_spp_stop_srv();
        s_spp_server_handle = 0;
    }
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    s_rx_buffer_len = 0;
    set_state(BT_CHESS_STATE_IDLE);
    s_role = BT_CHESS_ROLE_NONE;
}

void bt_chess_set_state_callback(bt_chess_state_callback_t callback)
{
    s_state_callback = callback;
}

void bt_chess_set_device_found_callback(bt_chess_device_found_callback_t callback)
{
    s_device_found_callback = callback;
}

void bt_chess_set_device_list_complete_callback(bt_chess_device_list_complete_callback_t callback)
{
    s_device_list_complete_callback = callback;
}

void bt_chess_set_connected_callback(bt_chess_connected_callback_t callback)
{
    s_connected_callback = callback;
}

void bt_chess_set_disconnected_callback(bt_chess_disconnected_callback_t callback)
{
    s_disconnected_callback = callback;
}

void bt_chess_set_invite_callback(bt_chess_invite_received_callback_t callback)
{
    s_invite_callback = callback;
}

void bt_chess_set_invite_accepted_callback(bt_chess_invite_accepted_callback_t callback)
{
    s_invite_accepted_callback = callback;
}

void bt_chess_set_move_callback(bt_chess_move_received_callback_t callback)
{
    s_move_callback = callback;
}

void bt_chess_set_game_over_callback(bt_chess_game_over_callback_t callback)
{
    s_game_over_callback = callback;
}

void bt_chess_set_chat_callback(bt_chess_chat_received_callback_t callback)
{
    s_chat_callback = callback;
}
