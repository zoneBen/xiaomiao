#include "bluetooth_game.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

#define BT_GAME_TAG "BT_GAME"
#define SPP_BUFFER_SIZE 256

// 内部状态
static bool s_initialized = false;
static BluetoothGameState s_state = BT_GAME_STATE_IDLE;
static BluetoothGameRole s_role = BT_GAME_ROLE_NONE;
static BluetoothGameType s_game_type = BT_GAME_CHESS;
static int s_local_side = 0;
static bool s_is_my_turn = true;
static uint32_t s_spp_handle = 0;
static uint32_t s_spp_server_handle = 0;
static uint8_t s_target_bda[6] = {0};

// 重连状态
static bool s_was_in_game = false;
static bool s_trying_reconnect = false;
static bool s_just_reconnected = false;
static int64_t s_just_reconnected_time = 0;
static uint8_t s_last_connected_bda[6] = {0};
static int s_reconnect_attempts = 0;

// 待确认移动和同步
static PendingGameMove s_pending_moves[MAX_PENDING_MOVES];
static int s_pending_count = 0;
static uint32_t s_next_sequence_num = 1;
static uint32_t s_last_acked_sequence_num = 0;
static bool s_sync_requested = false;

// 保存的游戏状态
static GenericGameState s_saved_game_state;

// 发现的设备列表
static FoundBluetoothDevice s_found_devices[MAX_FOUND_DEVICES];
static int s_found_device_count = 0;
static _lock_t s_bluetooth_lock;

// 回调函数指针
static bt_game_state_callback_t s_state_callback = NULL;
static bt_game_device_found_callback_t s_device_found_callback = NULL;
static bt_game_device_list_complete_callback_t s_device_list_complete_callback = NULL;
static bt_game_connected_callback_t s_connected_callback = NULL;
static bt_game_disconnected_callback_t s_disconnected_callback = NULL;
static bt_game_invite_received_callback_t s_invite_callback = NULL;
static bt_game_invite_accepted_callback_t s_invite_accepted_callback = NULL;
static bt_game_move_received_callback_t s_move_callback = NULL;
static bt_game_game_over_callback_t s_game_over_callback = NULL;
static bt_game_chat_received_callback_t s_chat_callback = NULL;
static bt_game_sync_received_callback_t s_sync_callback = NULL;

// 接收缓冲区
static uint8_t s_rx_buffer[SPP_BUFFER_SIZE];
static int s_rx_buffer_len = 0;

// 获取当前游戏的设备名称前缀
static const char* get_current_prefix(void) {
    switch (s_game_type) {
        case BT_GAME_XIANGQI:
            return BT_GAME_XIANGQI_PREFIX;
        case BT_GAME_GOMOKU:
            return BT_GAME_GOMOKU_PREFIX;
        case BT_GAME_GO:
            return BT_GAME_GO_PREFIX;
        case BT_GAME_JUNQI:
            return BT_GAME_JUNQI_PREFIX;
        case BT_GAME_CHESS:
        default:
            return BT_GAME_CHESS_PREFIX;
    }
}

// 获取当前游戏的SPP服务器名称
static const char* get_current_server_name(void) {
    switch (s_game_type) {
        case BT_GAME_XIANGQI:
            return "XC_XIANGQI_SERVER";
        case BT_GAME_GOMOKU:
            return "XC_GOMOKU_SERVER";
        case BT_GAME_GO:
            return "XC_GO_SERVER";
        case BT_GAME_JUNQI:
            return "XC_JUNQI_SERVER";
        case BT_GAME_CHESS:
        default:
            return "XC_CHESS_SERVER";
    }
}

// 工具函数：BD地址转字符串
static char* bda2str(const uint8_t* bda, char* str, size_t size) {
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }
    snprintf(str, size, "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return str;
}

// 从BD地址生成哈希
static void generate_hash_from_bda(const uint8_t* bda, char* hash, size_t hash_size) {
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

// 生成蓝牙设备名称
static void generate_device_name(char* name, size_t name_size) {
    const uint8_t* bda = esp_bt_dev_get_address();
    char hash[7];
    const char* prefix = get_current_prefix();
    if (bda == NULL) {
        snprintf(name, name_size, "%s000000", prefix);
        return;
    }
    generate_hash_from_bda(bda, hash, sizeof(hash));
    snprintf(name, name_size, "%s%s", prefix, hash);
}

// 检查是否是对应游戏的设备
static bool is_game_device(const char* name, const char* prefix, char* hash_out, size_t hash_size) {
    if (name == NULL || prefix == NULL) return false;
    size_t prefix_len = strlen(prefix);
    if (strlen(name) < prefix_len + 6) return false;
    if (strncmp(name, prefix, prefix_len) != 0) return false;
    if (hash_out != NULL && hash_size >= 7) {
        strncpy(hash_out, name + prefix_len, 6);
        hash_out[6] = '\0';
    }
    return true;
}

// 从EIR数据中解析设备名称
static bool get_name_from_eir(uint8_t* eir, char* bdname, uint8_t* bdname_len) {
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

// 设置状态并通知回调
static void set_state(BluetoothGameState new_state) {
    if (s_state != new_state) {
        ESP_LOGI(BT_GAME_TAG, "State changed: %d -> %d", s_state, new_state);
        s_state = new_state;
        if (s_state_callback) {
            s_state_callback(new_state);
        }
    }
}

// 发送数据
static bool send_data(const uint8_t* data, size_t len) {
    if (s_spp_handle == 0) {
        ESP_LOGE(BT_GAME_TAG, "Not connected, cannot send data");
        return false;
    }
    esp_err_t ret = esp_spp_write(s_spp_handle, len, (uint8_t*)data);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_GAME_TAG, "Failed to send data: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

// 解析接收到的消息
static void parse_message(const uint8_t* data, size_t len) {
    if (len < 1) return;
    uint8_t msg_type = data[0];
    const uint8_t* payload = data + 1;
    size_t payload_len = len - 1;

    switch (msg_type) {
        case BT_GAME_MSG_HELLO:
            ESP_LOGI(BT_GAME_TAG, "Received HELLO message");
            if (s_role == BT_GAME_ROLE_GUEST && payload_len >= 1) {
                if (!s_just_reconnected) {
                    s_local_side = payload[0];
                    s_is_my_turn = (s_local_side == 0);
                }
                bool was_reconnecting = s_just_reconnected;
                set_state(BT_GAME_STATE_GAME_READY);
                if (!was_reconnecting && s_invite_accepted_callback) {
                    s_invite_accepted_callback(s_local_side);
                }
            }
            break;

        case BT_GAME_MSG_INVITE:
            ESP_LOGI(BT_GAME_TAG, "Received INVITE message");
            if (s_invite_callback) {
                s_invite_callback();
            }
            break;

        case BT_GAME_MSG_ACCEPT:
            ESP_LOGI(BT_GAME_TAG, "Received ACCEPT message");
            if (s_role == BT_GAME_ROLE_GUEST && payload_len >= 1) {
                s_local_side = payload[0];
                s_is_my_turn = (s_local_side == 0);
                set_state(BT_GAME_STATE_GAME_READY);
                if (s_invite_accepted_callback) {
                    s_invite_accepted_callback(s_local_side);
                }
            }
            break;

        case BT_GAME_MSG_REJECT:
            ESP_LOGI(BT_GAME_TAG, "Received REJECT message");
            set_state(BT_GAME_STATE_IDLE);
            break;

        case BT_GAME_MSG_MOVE:
            ESP_LOGI(BT_GAME_TAG, "Received MOVE message");
            if (payload_len >= 9) {
                uint32_t seq_num = ((uint32_t)payload[0] << 24) |
                                   ((uint32_t)payload[1] << 16) |
                                   ((uint32_t)payload[2] << 8) |
                                   (uint32_t)payload[3];
                ESP_LOGI(BT_GAME_TAG, "Move seq=%"PRIu32, seq_num);
                int from_x = payload[4];
                int from_y = payload[5];
                int to_x = payload[6];
                int to_y = payload[7];
                int promotion = payload[8];

                uint8_t ack_msg[6];
                ack_msg[0] = 5;
                ack_msg[1] = BT_GAME_MSG_MOVE_ACK;
                ack_msg[2] = payload[0];
                ack_msg[3] = payload[1];
                ack_msg[4] = payload[2];
                ack_msg[5] = payload[3];
                send_data(ack_msg, 6);

                s_is_my_turn = true;
                if (s_move_callback) {
                    s_move_callback(from_x, from_y, to_x, to_y, promotion);
                }
            } else if (payload_len >= 5) {
                int from_x = payload[0];
                int from_y = payload[1];
                int to_x = payload[2];
                int to_y = payload[3];
                int promotion = payload[4];
                s_is_my_turn = true;
                if (s_move_callback) {
                    s_move_callback(from_x, from_y, to_x, to_y, promotion);
                }
            }
            break;

        case BT_GAME_MSG_MOVE_ACK:
            ESP_LOGI(BT_GAME_TAG, "Received MOVE_ACK message");
            if (payload_len >= 4) {
                uint32_t acked_seq = ((uint32_t)payload[0] << 24) |
                                     ((uint32_t)payload[1] << 16) |
                                     ((uint32_t)payload[2] << 8) |
                                     (uint32_t)payload[3];
                ESP_LOGI(BT_GAME_TAG, "Acked sequence: %"PRIu32, acked_seq);
                for (int i = 0; i < s_pending_count; i++) {
                    if (s_pending_moves[i].sequence_num == acked_seq && !s_pending_moves[i].acknowledged) {
                        s_pending_moves[i].acknowledged = true;
                        ESP_LOGI(BT_GAME_TAG, "Move %"PRIu32" acknowledged", acked_seq);
                        break;
                    }
                }
                int new_count = 0;
                for (int i = 0; i < s_pending_count; i++) {
                    if (!s_pending_moves[i].acknowledged) {
                        if (i != new_count) {
                            s_pending_moves[new_count] = s_pending_moves[i];
                        }
                        new_count++;
                    }
                }
                s_pending_count = new_count;
            }
            break;

        case BT_GAME_MSG_SYNC_REQUEST:
            ESP_LOGI(BT_GAME_TAG, "Received SYNC_REQUEST");
            if (s_role == BT_GAME_ROLE_HOST) {
                bt_game_send_game_sync(&s_saved_game_state);
            }
            break;

        case BT_GAME_MSG_SYNC:
            ESP_LOGI(BT_GAME_TAG, "Received SYNC message");
            if (payload_len > 0) {
                bt_game_process_game_sync(payload, payload_len);
            }
            break;

        case BT_GAME_MSG_GAME_OVER:
            ESP_LOGI(BT_GAME_TAG, "Received GAME_OVER message");
            if (payload_len >= 1) {
                int winner = payload[0];
                set_state(BT_GAME_STATE_GAME_OVER);
                if (s_game_over_callback) {
                    s_game_over_callback(winner);
                }
            }
            break;

        case BT_GAME_MSG_RESIGN:
            ESP_LOGI(BT_GAME_TAG, "Received RESIGN message");
            int resigner_side = (s_role == BT_GAME_ROLE_HOST) ? 1 : 0;
            int winner = (resigner_side == 0) ? 1 : 0;
            set_state(BT_GAME_STATE_GAME_OVER);
            if (s_game_over_callback) {
                s_game_over_callback(winner);
            }
            break;

        case BT_GAME_MSG_CHAT:
            ESP_LOGI(BT_GAME_TAG, "Received CHAT message");
            if (payload_len > 0 && s_chat_callback) {
                char chat_msg[256];
                size_t copy_len = (payload_len < 255) ? payload_len : 255;
                memcpy(chat_msg, payload, copy_len);
                chat_msg[copy_len] = '\0';
                s_chat_callback(chat_msg);
            }
            break;

        default:
            ESP_LOGW(BT_GAME_TAG, "Unknown message type: %d", msg_type);
            break;
    }
}

// 处理接收缓冲区
static void process_rx_buffer(void) {
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

// SPP事件回调
static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t* param) {
    char bda_str[18];
    ESP_LOGI(BT_GAME_TAG, "SPP callback event: %d", event);

    switch (event) {
        case ESP_SPP_INIT_EVT:
            ESP_LOGI(BT_GAME_TAG, "SPP initialized");
            break;

        case ESP_SPP_DISCOVERY_COMP_EVT:
            ESP_LOGI(BT_GAME_TAG, "SPP discovery complete, status: %d, scn_num: %d",
                     param->disc_comp.status, param->disc_comp.scn_num);
            if (param->disc_comp.status == ESP_SPP_SUCCESS && param->disc_comp.scn_num > 0) {
                uint8_t* connect_bda = (s_trying_reconnect && s_reconnect_attempts > 0) ?
                                      s_last_connected_bda : s_target_bda;
                esp_spp_connect(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_MASTER,
                               param->disc_comp.scn[0], connect_bda);
            } else {
                if (s_trying_reconnect) {
                    if (s_role == BT_GAME_ROLE_HOST) {
                        ESP_LOGI(BT_GAME_TAG, "Reconnect failed, restarting advertising...");
                        s_spp_server_handle = 0;
                        esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, get_current_server_name());
                    } else {
                        s_trying_reconnect = false;
                        s_was_in_game = false;
                        set_state(BT_GAME_STATE_IDLE);
                    }
                } else {
                    set_state(BT_GAME_STATE_IDLE);
                }
            }
            break;

        case ESP_SPP_OPEN_EVT:
        case ESP_SPP_SRV_OPEN_EVT:
            ESP_LOGI(BT_GAME_TAG, "SPP %s, handle: %"PRIu32", BDA: %s",
                     (event == ESP_SPP_OPEN_EVT) ? "opened" : "server opened",
                     (event == ESP_SPP_OPEN_EVT) ? param->open.handle : param->srv_open.handle,
                     bda2str((event == ESP_SPP_OPEN_EVT) ? param->open.rem_bda : param->srv_open.rem_bda,
                            bda_str, sizeof(bda_str)));

            if (event == ESP_SPP_OPEN_EVT) {
                memcpy(s_last_connected_bda, param->open.rem_bda, 6);
            } else {
                memcpy(s_last_connected_bda, param->srv_open.rem_bda, 6);
            }

            s_spp_handle = (event == ESP_SPP_OPEN_EVT) ? param->open.handle : param->srv_open.handle;
            bool is_reconnection = s_trying_reconnect;

            if (is_reconnection) {
                bool was_in_game = s_was_in_game;
                s_trying_reconnect = false;
                s_reconnect_attempts = 0;
                s_just_reconnected = true;
                s_just_reconnected_time = esp_timer_get_time();
                set_state(was_in_game ? BT_GAME_STATE_GAME_READY : BT_GAME_STATE_CONNECTED);
                s_was_in_game = false;

                if (s_role == BT_GAME_ROLE_HOST) {
                    s_local_side = 0;
                    s_is_my_turn = (s_saved_game_state.current_turn == 0);
                    bt_game_send_game_sync(&s_saved_game_state);
                    bt_game_resend_pending_moves();
                    s_just_reconnected = false;
                    if (s_state == BT_GAME_STATE_GAME_READY) {
                        set_state(BT_GAME_STATE_PLAYING);
                    }
                } else {
                    s_local_side = 1;
                    s_is_my_turn = (s_saved_game_state.current_turn == 1);
                    if (s_state == BT_GAME_STATE_GAME_READY) {
                        set_state(BT_GAME_STATE_PLAYING);
                    }
                    bt_game_request_game_sync();
                    bt_game_resend_pending_moves();
                }
            } else {
                set_state(BT_GAME_STATE_CONNECTED);
            }

            if (s_connected_callback) {
                s_connected_callback();
            }

            if (s_role == BT_GAME_ROLE_HOST && !is_reconnection) {
                s_local_side = 0;
                s_is_my_turn = true;
                uint8_t msg[3];
                msg[0] = 2;
                msg[1] = BT_GAME_MSG_HELLO;
                msg[2] = 1;
                send_data(msg, 3);
                if (s_invite_callback) {
                    s_invite_callback();
                }
            }
            break;

        case ESP_SPP_CLOSE_EVT:
            ESP_LOGI(BT_GAME_TAG, "SPP closed, handle: %"PRIu32, param->close.handle);
            if (s_spp_handle == param->close.handle) {
                s_spp_handle = 0;
            }
            s_rx_buffer_len = 0;

            if (s_state >= BT_GAME_STATE_GAME_READY && s_state <= BT_GAME_STATE_GAME_OVER) {
                s_was_in_game = true;
                s_reconnect_attempts = 0;
                set_state(BT_GAME_STATE_RECONNECTING);
                s_trying_reconnect = true;

                if (s_role == BT_GAME_ROLE_HOST) {
                    ESP_LOGI(BT_GAME_TAG, "Host: Restarting server, waiting for guest to reconnect");
                    if (s_spp_server_handle != 0) {
                        esp_spp_stop_srv();
                        s_spp_server_handle = 0;
                    }
                    esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, get_current_server_name());
                } else {
                    ESP_LOGI(BT_GAME_TAG, "Guest: Disconnected, waiting for user to press A to reconnect");
                    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
                }
            } else {
                s_was_in_game = false;
                s_trying_reconnect = false;
                set_state(BT_GAME_STATE_IDLE);
            }

            if (s_disconnected_callback) {
                s_disconnected_callback();
            }
            break;

        case ESP_SPP_START_EVT:
            ESP_LOGI(BT_GAME_TAG, "SPP server started, handle: %"PRIu32, param->start.handle);
            s_spp_server_handle = param->start.handle;
            char device_name[BT_GAME_NAME_LEN];
            generate_device_name(device_name, sizeof(device_name));
            ESP_LOGI(BT_GAME_TAG, "Setting device name for advertising: %s", device_name);
            esp_bt_gap_set_device_name(device_name);
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            if (!s_trying_reconnect) {
                set_state(BT_GAME_STATE_ADVERTISING);
            }
            break;

        case ESP_SPP_DATA_IND_EVT:
            ESP_LOGI(BT_GAME_TAG, "SPP data received, len: %d", param->data_ind.len);
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
            ESP_LOGI(BT_GAME_TAG, "SPP congestion: %d", param->cong.cong);
            break;

        default:
            break;
    }
}

// GAP事件回调
static void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT:
            ESP_LOGI(BT_GAME_TAG, "Discovery result");
            char bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
            uint8_t bdname_len = 0;
            const char* current_prefix = get_current_prefix();
            ESP_LOGI(BT_GAME_TAG, "Looking for devices with prefix: %s", current_prefix);

            for (int i = 0; i < param->disc_res.num_prop; i++) {
                if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_EIR &&
                    get_name_from_eir(param->disc_res.prop[i].val, bdname, &bdname_len)) {
                    ESP_LOGI(BT_GAME_TAG, "Found device: %s", bdname);
                    char hash[7];
                    if (is_game_device(bdname, current_prefix, hash, sizeof(hash))) {
                        ESP_LOGI(BT_GAME_TAG, "Found matching game device: %s (hash: %s)", bdname, hash);
                        _lock_acquire(&s_bluetooth_lock);
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
                            strncpy(dev->name, bdname, BT_GAME_NAME_LEN - 1);
                            dev->name[BT_GAME_NAME_LEN - 1] = '\0';
                            strncpy(dev->hash, hash, 6);
                            dev->hash[6] = '\0';
                            int current_count = s_found_device_count;
                            s_found_device_count++;
                            _lock_release(&s_bluetooth_lock);

                            ESP_LOGI(BT_GAME_TAG, "Added device to list, total: %d", current_count + 1);
                            if (s_device_found_callback) {
                                s_device_found_callback(dev, current_count);
                            }
                        } else {
                            _lock_release(&s_bluetooth_lock);
                        }
                    }
                }
            }
            break;

        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            ESP_LOGI(BT_GAME_TAG, "Discovery state changed: %d", param->disc_st_chg.state);
            _lock_acquire(&s_bluetooth_lock);
            int count = s_found_device_count;
            _lock_release(&s_bluetooth_lock);
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED && s_state == BT_GAME_STATE_SCANNING) {
                if (s_device_list_complete_callback) {
                    s_device_list_complete_callback(count);
                }
            }
            break;

        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(BT_GAME_TAG, "Authentication successful: %s", param->auth_cmpl.device_name);
            } else {
                ESP_LOGE(BT_GAME_TAG, "Authentication failed: %d", param->auth_cmpl.stat);
            }
            break;

        case ESP_BT_GAP_PIN_REQ_EVT:
            ESP_LOGI(BT_GAME_TAG, "PIN request, min 16 digit: %d", param->pin_req.min_16_digit);
            esp_bt_pin_code_t pin_code = {'1','2','3','4'};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
            break;

        case ESP_BT_GAP_CFM_REQ_EVT:
            ESP_LOGI(BT_GAME_TAG, "SSP confirm request: %06"PRIu32, param->cfm_req.num_val);
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;

        case ESP_BT_GAP_KEY_NOTIF_EVT:
            ESP_LOGI(BT_GAME_TAG, "SSP key notification: %06"PRIu32, param->key_notif.passkey);
            break;

        case ESP_BT_GAP_KEY_REQ_EVT:
            ESP_LOGI(BT_GAME_TAG, "SSP key request");
            break;

        default:
            ESP_LOGI(BT_GAME_TAG, "GAP event: %d", event);
            break;
    }
}

// ============== 公共API实现 ==============

void bt_game_set_type(BluetoothGameType game_type) {
    ESP_LOGI(BT_GAME_TAG, "Setting game type: %d", game_type);
    s_game_type = game_type;

    if (s_initialized) {
        char device_name[BT_GAME_NAME_LEN];
        generate_device_name(device_name, sizeof(device_name));
        esp_bt_gap_set_device_name(device_name);
        ESP_LOGI(BT_GAME_TAG, "Updated device name to: %s", device_name);
    }
}

BluetoothGameType bt_game_get_type(void) {
    return s_game_type;
}

void bt_game_init(void) {
    if (s_initialized) {
        ESP_LOGW(BT_GAME_TAG, "Already initialized, skipping");
        return;
    }
    ESP_LOGI(BT_GAME_TAG, "Initializing Bluetooth game module");
    _lock_init(&s_bluetooth_lock);
    s_state = BT_GAME_STATE_IDLE;
    s_role = BT_GAME_ROLE_NONE;
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
        ESP_LOGE(BT_GAME_TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(BT_GAME_TAG, "Controller init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(BT_GAME_TAG, "Controller enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(BT_GAME_TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(BT_GAME_TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_gap_register_callback(esp_bt_gap_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_GAME_TAG, "GAP register failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_spp_register_callback(esp_spp_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_GAME_TAG, "SPP register failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_spp_cfg_t bt_spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0,
    };
    ret = esp_spp_enhanced_init(&bt_spp_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(BT_GAME_TAG, "SPP init failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_pin_code_t pin_code = {'1','2','3','4'};
    esp_bt_gap_set_pin(pin_type, 4, pin_code);

    s_pending_count = 0;
    s_next_sequence_num = 1;
    s_last_acked_sequence_num = 0;
    s_sync_requested = false;

    memset(&s_saved_game_state, 0, sizeof(s_saved_game_state));

    char bda_str[18];
    char hash[7];
    const uint8_t* bda = esp_bt_dev_get_address();
    if (bda != NULL) {
        generate_hash_from_bda(bda, hash, sizeof(hash));
        ESP_LOGI(BT_GAME_TAG, "Own BDA: %s, Hash: %s", bda2str(bda, bda_str, sizeof(bda_str)), hash);
    } else {
        ESP_LOGW(BT_GAME_TAG, "BDA not available yet");
    }

    s_initialized = true;
}

void bt_game_deinit(void) {
    if (!s_initialized) {
        return;
    }
    ESP_LOGI(BT_GAME_TAG, "Deinitializing Bluetooth game module");
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
    s_state = BT_GAME_STATE_IDLE;
    s_role = BT_GAME_ROLE_NONE;
    s_found_device_count = 0;
    s_initialized = false;
}

BluetoothGameState bt_game_get_state(void) {
    return s_state;
}

BluetoothGameRole bt_game_get_role(void) {
    return s_role;
}

int bt_game_get_local_side(void) {
    return s_local_side;
}

bool bt_game_is_my_turn(void) {
    return s_is_my_turn;
}

bool bt_game_start_hosting(void) {
    ESP_LOGI(BT_GAME_TAG, "Starting hosting for game type: %d", s_game_type);
    if (!s_initialized) {
        ESP_LOGE(BT_GAME_TAG, "Bluetooth not initialized");
        return false;
    }
    if (s_spp_server_handle != 0) {
        ESP_LOGW(BT_GAME_TAG, "Server already running, stopping first");
        bt_game_stop_hosting();
    }
    if (s_state != BT_GAME_STATE_IDLE) {
        ESP_LOGW(BT_GAME_TAG, "Cannot host in state: %d", s_state);
        return false;
    }
    s_role = BT_GAME_ROLE_HOST;
    s_found_device_count = 0;
    esp_err_t ret = esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, get_current_server_name());
    if (ret != ESP_OK) {
        ESP_LOGE(BT_GAME_TAG, "Failed to start SPP server: %s", esp_err_to_name(ret));
        s_role = BT_GAME_ROLE_NONE;
        return false;
    }
    return true;
}

void bt_game_stop_hosting(void) {
    ESP_LOGI(BT_GAME_TAG, "Stopping hosting");
    if (s_spp_server_handle != 0) {
        esp_spp_stop_srv();
        s_spp_server_handle = 0;
    }
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    if (s_state == BT_GAME_STATE_ADVERTISING || s_role == BT_GAME_ROLE_HOST) {
        set_state(BT_GAME_STATE_IDLE);
        s_role = BT_GAME_ROLE_NONE;
    }
}

bool bt_game_accept_connection(void) {
    ESP_LOGI(BT_GAME_TAG, "Accepting connection");
    if (s_state != BT_GAME_STATE_CONNECTED) {
        ESP_LOGW(BT_GAME_TAG, "Cannot accept in state: %d", s_state);
        return false;
    }
    uint8_t msg[3];
    msg[0] = 2;
    msg[1] = BT_GAME_MSG_ACCEPT;
    msg[2] = 0;
    if (!send_data(msg, 3)) {
        return false;
    }
    s_local_side = 0;
    s_is_my_turn = true;
    set_state(BT_GAME_STATE_GAME_READY);
    return true;
}

void bt_game_reject_connection(void) {
    ESP_LOGI(BT_GAME_TAG, "Rejecting connection");
    if (s_spp_handle != 0) {
        uint8_t msg[2];
        msg[0] = 1;
        msg[1] = BT_GAME_MSG_REJECT;
        send_data(msg, 2);
        esp_spp_disconnect(s_spp_handle);
    }
    set_state(BT_GAME_STATE_IDLE);
    s_role = BT_GAME_ROLE_NONE;
}

bool bt_game_start_scanning(void) {
    ESP_LOGI(BT_GAME_TAG, "Starting scan");
    if (!s_initialized) {
        ESP_LOGE(BT_GAME_TAG, "Bluetooth not initialized");
        return false;
    }
    if (s_state != BT_GAME_STATE_IDLE) {
        ESP_LOGW(BT_GAME_TAG, "Cannot scan in state: %d", s_state);
        return false;
    }
    _lock_acquire(&s_bluetooth_lock);
    s_role = BT_GAME_ROLE_GUEST;
    s_found_device_count = 0;
    _lock_release(&s_bluetooth_lock);

    char device_name[BT_GAME_NAME_LEN];
    generate_device_name(device_name, sizeof(device_name));
    esp_bt_gap_set_device_name(device_name);
    set_state(BT_GAME_STATE_SCANNING);

    esp_err_t ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 30, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_GAME_TAG, "Failed to start discovery: %s", esp_err_to_name(ret));
        set_state(BT_GAME_STATE_IDLE);
        _lock_acquire(&s_bluetooth_lock);
        s_role = BT_GAME_ROLE_NONE;
        _lock_release(&s_bluetooth_lock);
        return false;
    }
    return true;
}

void bt_game_stop_scanning(void) {
    ESP_LOGI(BT_GAME_TAG, "Stopping scan");
    if (s_state == BT_GAME_STATE_SCANNING) {
        ESP_LOGI(BT_GAME_TAG, "Cancelling discovery...");
        esp_bt_gap_cancel_discovery();
        ESP_LOGI(BT_GAME_TAG, "Setting state to idle...");
        set_state(BT_GAME_STATE_IDLE);
        ESP_LOGI(BT_GAME_TAG, "Acquiring bluetooth lock...");
        _lock_acquire(&s_bluetooth_lock);
        ESP_LOGI(BT_GAME_TAG, "Updating role...");
        s_role = BT_GAME_ROLE_NONE;
        ESP_LOGI(BT_GAME_TAG, "Releasing bluetooth lock...");
        _lock_release(&s_bluetooth_lock);
        ESP_LOGI(BT_GAME_TAG, "Scan stopped successfully");
    }
}

int bt_game_get_found_device_count(void) {
    _lock_acquire(&s_bluetooth_lock);
    int count = s_found_device_count;
    _lock_release(&s_bluetooth_lock);
    return count;
}

const FoundBluetoothDevice* bt_game_get_found_device(int index) {
    _lock_acquire(&s_bluetooth_lock);
    const FoundBluetoothDevice* device = NULL;
    if (index >= 0 && index < s_found_device_count) {
        device = &s_found_devices[index];
    }
    _lock_release(&s_bluetooth_lock);
    return device;
}

int bt_game_copy_found_devices(FoundBluetoothDevice* buffer, size_t max_count) {
    _lock_acquire(&s_bluetooth_lock);
    int count = (s_found_device_count < max_count) ? s_found_device_count : max_count;
    if (buffer && count > 0) {
        memcpy(buffer, s_found_devices, count * sizeof(FoundBluetoothDevice));
    }
    _lock_release(&s_bluetooth_lock);
    return count;
}

bool bt_game_connect_to_device(int index) {
    ESP_LOGI(BT_GAME_TAG, "Connecting to device index: %d", index);
    if (!s_initialized) {
        ESP_LOGE(BT_GAME_TAG, "Bluetooth not initialized");
        return false;
    }
    _lock_acquire(&s_bluetooth_lock);
    if (index < 0 || index >= s_found_device_count) {
        _lock_release(&s_bluetooth_lock);
        ESP_LOGE(BT_GAME_TAG, "Invalid device index");
        return false;
    }
    if (s_state != BT_GAME_STATE_SCANNING) {
        _lock_release(&s_bluetooth_lock);
        ESP_LOGW(BT_GAME_TAG, "Cannot connect in state: %d", s_state);
        return false;
    }
    memcpy(s_target_bda, s_found_devices[index].bda, 6);
    _lock_release(&s_bluetooth_lock);

    esp_bt_gap_cancel_discovery();
    set_state(BT_GAME_STATE_CONNECTING);
    esp_err_t ret = esp_spp_start_discovery(s_target_bda);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_GAME_TAG, "Failed to start SPP discovery: %s", esp_err_to_name(ret));
        set_state(BT_GAME_STATE_IDLE);
        return false;
    }
    return true;
}

bool bt_game_connect_to_hash(const char* hash) {
    ESP_LOGI(BT_GAME_TAG, "Connecting to hash: %s", hash);
    if (hash == NULL || strlen(hash) != 6) {
        ESP_LOGE(BT_GAME_TAG, "Invalid hash");
        return false;
    }
    for (int i = 0; i < s_found_device_count; i++) {
        if (strcasecmp(s_found_devices[i].hash, hash) == 0) {
            return bt_game_connect_to_device(i);
        }
    }
    ESP_LOGW(BT_GAME_TAG, "Device with hash %s not found", hash);
    return false;
}

bool bt_game_send_move(int from_x, int from_y, int to_x, int to_y, int promotion) {
    ESP_LOGI(BT_GAME_TAG, "Sending move: %d,%d -> %d,%d (promo: %d)", from_x, from_y, to_x, to_y, promotion);
    if (s_spp_handle == 0) {
        ESP_LOGE(BT_GAME_TAG, "Not connected");
        return false;
    }
    uint8_t msg[11];
    msg[0] = 10;
    msg[1] = BT_GAME_MSG_MOVE;
    msg[2] = (s_next_sequence_num >> 24) & 0xFF;
    msg[3] = (s_next_sequence_num >> 16) & 0xFF;
    msg[4] = (s_next_sequence_num >> 8) & 0xFF;
    msg[5] = s_next_sequence_num & 0xFF;
    msg[6] = from_x & 0xFF;
    msg[7] = from_y & 0xFF;
    msg[8] = to_x & 0xFF;
    msg[9] = to_y & 0xFF;
    msg[10] = promotion & 0xFF;

    if (send_data(msg, 11)) {
        if (s_pending_count < MAX_PENDING_MOVES) {
            s_pending_moves[s_pending_count].from_x = from_x;
            s_pending_moves[s_pending_count].from_y = from_y;
            s_pending_moves[s_pending_count].to_x = to_x;
            s_pending_moves[s_pending_count].to_y = to_y;
            s_pending_moves[s_pending_count].promotion = promotion;
            s_pending_moves[s_pending_count].sequence_num = s_next_sequence_num;
            s_pending_moves[s_pending_count].acknowledged = false;
            s_pending_count++;
        }
        s_next_sequence_num++;
        s_is_my_turn = false;
        if (s_state == BT_GAME_STATE_GAME_READY) {
            set_state(BT_GAME_STATE_PLAYING);
        }
        return true;
    }
    return false;
}

bool bt_game_send_sync(const GenericGameState* state) {
    return bt_game_send_game_sync(state);
}

bool bt_game_send_resign(void) {
    ESP_LOGI(BT_GAME_TAG, "Sending resignation");
    if (s_spp_handle == 0) {
        return false;
    }
    uint8_t msg[2];
    msg[0] = 1;
    msg[1] = BT_GAME_MSG_RESIGN;
    if (send_data(msg, 2)) {
        int winner = (s_local_side == 0) ? 1 : 0;
        set_state(BT_GAME_STATE_GAME_OVER);
        if (s_game_over_callback) {
            s_game_over_callback(winner);
        }
        return true;
    }
    return false;
}

bool bt_game_send_chat(const char* message) {
    if (s_spp_handle == 0 || message == NULL) {
        return false;
    }
    size_t msg_len = strlen(message);
    if (msg_len > 250) {
        msg_len = 250;
    }
    uint8_t msg[256];
    msg[0] = 1 + msg_len;
    msg[1] = BT_GAME_MSG_CHAT;
    memcpy(&msg[2], message, msg_len);
    return send_data(msg, 2 + msg_len);
}

bool bt_game_send_game_over(int winner) {
    ESP_LOGI(BT_GAME_TAG, "Sending game over, winner: %d", winner);
    if (s_spp_handle == 0) {
        return false;
    }
    uint8_t msg[3];
    msg[0] = 2;
    msg[1] = BT_GAME_MSG_GAME_OVER;
    msg[2] = winner;
    if (send_data(msg, 3)) {
        set_state(BT_GAME_STATE_GAME_OVER);
        return true;
    }
    return false;
}

void bt_game_disconnect(void) {
    ESP_LOGI(BT_GAME_TAG, "Disconnecting");
    s_trying_reconnect = false;
    s_was_in_game = false;
    s_reconnect_attempts = 0;
    memset(s_last_connected_bda, 0, 6);

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
    set_state(BT_GAME_STATE_IDLE);
    s_role = BT_GAME_ROLE_NONE;
}

bool bt_game_try_reconnect(void) {
    if (!s_initialized) {
        return false;
    }
    if (!s_trying_reconnect) {
        return false;
    }
    ESP_LOGI(BT_GAME_TAG, "Manual reconnect attempt to device");
    s_reconnect_attempts++;
    if (s_role == BT_GAME_ROLE_HOST) {
        return true;
    } else {
        if (s_spp_server_handle != 0) {
            esp_spp_stop_srv();
            s_spp_server_handle = 0;
        }
        esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        memcpy(s_target_bda, s_last_connected_bda, 6);
        set_state(BT_GAME_STATE_CONNECTING);
        esp_err_t ret = esp_spp_start_discovery(s_last_connected_bda);
        return (ret == ESP_OK);
    }
}

bool bt_game_was_in_game(void) {
    return s_was_in_game || s_trying_reconnect;
}

void bt_game_save_game_state(const GenericGameState* state) {
    if (state != NULL) {
        memcpy(&s_saved_game_state, state, sizeof(GenericGameState));
    }
}

bool bt_game_send_game_sync(const GenericGameState* state) {
    ESP_LOGI(BT_GAME_TAG, "Sending full game sync");
    if (s_spp_handle == 0 || state == NULL) {
        return false;
    }

    uint8_t sync_msg[256];
    size_t msg_len = 0;
    sync_msg[msg_len++] = 0;  // 长度占位符
    sync_msg[msg_len++] = BT_GAME_MSG_SYNC;

    memcpy(&sync_msg[msg_len], state->board_data, state->board_data_len);
    msg_len += state->board_data_len;

    sync_msg[msg_len++] = state->last_from_x;
    sync_msg[msg_len++] = state->last_from_y;
    sync_msg[msg_len++] = state->last_to_x;
    sync_msg[msg_len++] = state->last_to_y;
    sync_msg[msg_len++] = state->current_turn;
    sync_msg[msg_len++] = state->fullmove_number;
    sync_msg[msg_len++] = state->halfmove_clock;
    sync_msg[msg_len++] = state->game_over;
    sync_msg[msg_len++] = state->winner;

    sync_msg[0] = msg_len - 1;

    return send_data(sync_msg, msg_len);
}

void bt_game_request_game_sync(void) {
    ESP_LOGI(BT_GAME_TAG, "Requesting game sync");
    if (s_spp_handle == 0) {
        return;
    }
    uint8_t msg[2];
    msg[0] = 1;
    msg[1] = BT_GAME_MSG_SYNC_REQUEST;
    send_data(msg, 2);
    s_sync_requested = true;
}

void bt_game_process_game_sync(const uint8_t* data, size_t len) {
    ESP_LOGI(BT_GAME_TAG, "Processing game sync, len=%d", len);
    if (len < 4) {
        return;
    }

    GenericGameState temp_state;
    memset(&temp_state, 0, sizeof(temp_state));

    size_t offset = 0;
    temp_state.board_data_len = len - 9;  // 减去最后9字节状态
    if (temp_state.board_data_len > 256) temp_state.board_data_len = 256;
    memcpy(temp_state.board_data, data, temp_state.board_data_len);
    offset = temp_state.board_data_len;

    temp_state.last_from_x = data[offset++];
    temp_state.last_from_y = data[offset++];
    temp_state.last_to_x = data[offset++];
    temp_state.last_to_y = data[offset++];
    temp_state.current_turn = data[offset++];
    temp_state.fullmove_number = data[offset++];
    temp_state.halfmove_clock = data[offset++];
    temp_state.game_over = data[offset++];
    temp_state.winner = data[offset++];

    s_is_my_turn = (s_role == BT_GAME_ROLE_HOST) ?
                   (temp_state.current_turn == 0) :
                   (temp_state.current_turn == 1);

    memcpy(&s_saved_game_state, &temp_state, sizeof(GenericGameState));

    ESP_LOGI(BT_GAME_TAG, "Sync complete: fullmove=%d, turn=%d, my_turn=%s",
             temp_state.fullmove_number, temp_state.current_turn,
             s_is_my_turn ? "YES" : "NO");

    if (s_sync_callback) {
        s_sync_callback(&temp_state);
    }

    s_sync_requested = false;
    s_just_reconnected = false;

    if (s_state == BT_GAME_STATE_GAME_READY) {
        set_state(BT_GAME_STATE_PLAYING);
    }
}

bool bt_game_has_pending_moves(void) {
    return s_pending_count > 0;
}

void bt_game_resend_pending_moves(void) {
    ESP_LOGI(BT_GAME_TAG, "Resending %d pending moves", s_pending_count);
    if (s_spp_handle == 0 || s_pending_count == 0) {
        return;
    }

    for (int i = 0; i < s_pending_count; i++) {
        if (!s_pending_moves[i].acknowledged) {
            PendingGameMove* pm = &s_pending_moves[i];
            uint8_t msg[11];
            msg[0] = 10;
            msg[1] = BT_GAME_MSG_MOVE;
            msg[2] = (pm->sequence_num >> 24) & 0xFF;
            msg[3] = (pm->sequence_num >> 16) & 0xFF;
            msg[4] = (pm->sequence_num >> 8) & 0xFF;
            msg[5] = pm->sequence_num & 0xFF;
            msg[6] = pm->from_x & 0xFF;
            msg[7] = pm->from_y & 0xFF;
            msg[8] = pm->to_x & 0xFF;
            msg[9] = pm->to_y & 0xFF;
            msg[10] = pm->promotion & 0xFF;
            send_data(msg, 11);
            ESP_LOGI(BT_GAME_TAG, "Resent move seq=%"PRIu32, pm->sequence_num);
        }
    }
}

void bt_game_set_state_callback(bt_game_state_callback_t callback) {
    s_state_callback = callback;
}

void bt_game_set_device_found_callback(bt_game_device_found_callback_t callback) {
    s_device_found_callback = callback;
}

void bt_game_set_device_list_complete_callback(bt_game_device_list_complete_callback_t callback) {
    s_device_list_complete_callback = callback;
}

void bt_game_set_connected_callback(bt_game_connected_callback_t callback) {
    s_connected_callback = callback;
}

void bt_game_set_disconnected_callback(bt_game_disconnected_callback_t callback) {
    s_disconnected_callback = callback;
}

void bt_game_set_invite_callback(bt_game_invite_received_callback_t callback) {
    s_invite_callback = callback;
}

void bt_game_set_invite_accepted_callback(bt_game_invite_accepted_callback_t callback) {
    s_invite_accepted_callback = callback;
}

void bt_game_set_move_callback(bt_game_move_received_callback_t callback) {
    s_move_callback = callback;
}

void bt_game_set_game_over_callback(bt_game_game_over_callback_t callback) {
    s_game_over_callback = callback;
}

void bt_game_set_chat_callback(bt_game_chat_received_callback_t callback) {
    s_chat_callback = callback;
}

void bt_game_set_sync_callback(bt_game_sync_received_callback_t callback) {
    s_sync_callback = callback;
}

void bt_game_debug_dump(void) {
    ESP_LOGI(BT_GAME_TAG, "DEBUG: state=%d, role=%d, local_side=%d, is_my_turn=%d",
             s_state, s_role, s_local_side, s_is_my_turn);
    ESP_LOGI(BT_GAME_TAG, "DEBUG: spp_handle=%"PRIu32", spp_server_handle=%"PRIu32,
             s_spp_handle, s_spp_server_handle);
    ESP_LOGI(BT_GAME_TAG, "DEBUG: trying_reconnect=%d, was_in_game=%d, just_reconnected=%d",
             s_trying_reconnect, s_was_in_game, s_just_reconnected);
}
