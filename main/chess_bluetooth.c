#include "chess_bluetooth.h"
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

#define BT_CHESS_TAG "CHESS_BT"
#define BT_CHESS_SERVER_NAME "XC_CHESS_SERVER"
#define BT_XIANGQI_SERVER_NAME "XC_XIANGQI_SERVER"
#define SPP_BUFFER_SIZE 256

static bool s_initialized = false;
static BluetoothChessState s_state = BT_CHESS_STATE_IDLE;
static BluetoothChessRole s_role = BT_CHESS_ROLE_NONE;
static BluetoothGameType s_game_type = BT_GAME_CHESS;  // 默认是国际象棋
static ChessColor s_local_color = COLOR_WHITE;
static bool s_is_my_turn = true;
static uint32_t s_spp_handle = 0;
static uint32_t s_spp_server_handle = 0;
static uint8_t s_target_bda[6] = {0};
// Reconnection state
static bool s_was_in_game = false;
static bool s_trying_reconnect = false;
static bool s_just_reconnected = false;  // 标志：刚完成重连
static int64_t s_just_reconnected_time = 0;  // 记录设置重连标志的时间
static uint8_t s_last_connected_bda[6] = {0};
static int s_reconnect_attempts = 0;
// Pending moves and sync
static PendingMove s_pending_moves[MAX_PENDING_MOVES];
static int s_pending_count = 0;
static uint32_t s_next_sequence_num = 1;
static uint32_t s_last_acked_sequence = 0;
static bool s_sync_requested = false;
// Last saved game state for sync
static ChessGame s_saved_game_state;

static FoundBluetoothDevice s_found_devices[MAX_FOUND_DEVICES];
static int s_found_device_count = 0;
static _lock_t s_bluetooth_lock;  // Lock to protect Bluetooth data

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
static bt_chess_sync_received_callback_t s_sync_callback = NULL;

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

static const char* get_current_prefix(void)
{
    switch (s_game_type) {
        case BT_GAME_XIANGQI:
            return BT_XIANGQI_PREFIX;
        case BT_GAME_GOMOKU:
            return BT_GOMOKU_PREFIX;
        case BT_GAME_GO:
            return BT_GO_PREFIX;
        case BT_GAME_JUNQI:
            return BT_JUNQI_PREFIX;
        case BT_GAME_CHESS:
        default:
            return BT_CHESS_PREFIX;
    }
}

static const char* get_current_server_name(void)
{
    switch (s_game_type) {
        case BT_GAME_XIANGQI:
            return BT_XIANGQI_SERVER_NAME;
        case BT_GAME_CHESS:
        default:
            return BT_CHESS_SERVER_NAME;
    }
}

static bool is_game_device(const char* name, const char* prefix, char* hash_out, size_t hash_size)
{
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

static void generate_device_name(char* name, size_t name_size)
{
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

static bool is_chess_device(const char* name, char* hash_out, size_t hash_size)
{
    return is_game_device(name, get_current_prefix(), hash_out, hash_size);
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
        // 注意：回调可能会尝试获取其他锁，不要在持有蓝牙锁时调用！
        // 这里我们不持有锁，所以安全
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
            if (s_role == BT_CHESS_ROLE_GUEST && payload_len >= 1) {
                // 只有在非重连状态下才设置颜色和回合
                if (!s_just_reconnected) {
                    s_local_color = payload[0];
                    s_is_my_turn = (s_local_color == COLOR_WHITE);
                }
                // 如果是在重连状态，只设置状态，不回调（避免触发屏幕切换）
                bool was_reconnecting = s_just_reconnected;
                set_state(BT_CHESS_STATE_GAME_READY);
                if (!was_reconnecting && s_invite_accepted_callback) {
                    s_invite_accepted_callback(s_local_color);
                }
                // 重连时，清除标志已经在 sync 处理中完成
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
            if (payload_len >= 9) {
                // 新格式：4字节序列号 + 5字节移动数据
                uint32_t seq_num = ((uint32_t)payload[0] << 24) |
                                   ((uint32_t)payload[1] << 16) |
                                   ((uint32_t)payload[2] << 8) |
                                   (uint32_t)payload[3];
                (void)seq_num;  // 确保被使用
                ESP_LOGI(BT_CHESS_TAG, "Move seq=%"PRIu32, seq_num);
                int from_x = payload[4];
                int from_y = payload[5];
                int to_x = payload[6];
                int to_y = payload[7];
                ChessPieceType promotion = payload[8];

                // 发送确认
                uint8_t ack_msg[6];
                ack_msg[0] = 5;
                ack_msg[1] = BT_CHESS_MSG_MOVE_ACK;
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
                // 兼容旧格式
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
        case BT_CHESS_MSG_MOVE_ACK:
            ESP_LOGI(BT_CHESS_TAG, "Received MOVE_ACK message");
            if (payload_len >= 4) {
                uint32_t acked_seq = ((uint32_t)payload[0] << 24) |
                                     ((uint32_t)payload[1] << 16) |
                                     ((uint32_t)payload[2] << 8) |
                                     (uint32_t)payload[3];
                ESP_LOGI(BT_CHESS_TAG, "Acked sequence: %"PRIu32, acked_seq);
                // 标记为已确认
                for (int i = 0; i < s_pending_count; i++) {
                    if (s_pending_moves[i].sequence_num == acked_seq && !s_pending_moves[i].acknowledged) {
                        s_pending_moves[i].acknowledged = true;
                        ESP_LOGI(BT_CHESS_TAG, "Move %"PRIu32" acknowledged", acked_seq);
                        break;
                    }
                }
                // 清理已确认的移动
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
        case BT_CHESS_MSG_SYNC_REQUEST:
            ESP_LOGI(BT_CHESS_TAG, "Received SYNC_REQUEST");
            // 主设备收到同步请求，发送当前棋盘状态
            if (s_role == BT_CHESS_ROLE_HOST && s_game_over_callback) {
                // 通过回调让 UI 发送同步
                bt_chess_send_game_sync(&s_saved_game_state);
            }
            break;
        case BT_CHESS_MSG_SYNC:
            ESP_LOGI(BT_CHESS_TAG, "Received SYNC message");
            if (payload_len > 0) {
                bt_chess_process_game_sync(payload, payload_len);
            }
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
    ESP_LOGI(BT_CHESS_TAG, "SPP callback event: %d", event);
    switch (event) {
        case ESP_SPP_INIT_EVT:
            ESP_LOGI(BT_CHESS_TAG, "SPP initialized");
            break;
        case ESP_SPP_DISCOVERY_COMP_EVT:
            ESP_LOGI(BT_CHESS_TAG, "SPP discovery complete, status: %d, scn_num: %d",
                     param->disc_comp.status, param->disc_comp.scn_num);
            if (param->disc_comp.status == ESP_SPP_SUCCESS && param->disc_comp.scn_num > 0) {
                // Use saved target BDA or last connected BDA
                uint8_t* connect_bda = (s_trying_reconnect && s_reconnect_attempts > 0) ?
                                        s_last_connected_bda : s_target_bda;
                esp_spp_connect(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_MASTER,
                               param->disc_comp.scn[0], connect_bda);
            } else {
                if (s_trying_reconnect) {
                    // If discovery failed, reset to advertising for host or wait for scan for guest
                    if (s_role == BT_CHESS_ROLE_HOST) {
                        // Host should restart advertising
                        ESP_LOGI(BT_CHESS_TAG, "Reconnect failed, restarting advertising...");
                        s_spp_server_handle = 0;
                        esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, BT_CHESS_SERVER_NAME);
                    } else {
                        // Guest should go back to idle
                        s_trying_reconnect = false;
                        s_was_in_game = false;
                        set_state(BT_CHESS_STATE_IDLE);
                    }
                } else {
                    set_state(BT_CHESS_STATE_IDLE);
                }
            }
            break;
        case ESP_SPP_OPEN_EVT:
        case ESP_SPP_SRV_OPEN_EVT:
            ESP_LOGI(BT_CHESS_TAG, "SPP %s, handle: %"PRIu32", BDA: %s",
                     (event == ESP_SPP_OPEN_EVT) ? "opened" : "server opened",
                     (event == ESP_SPP_OPEN_EVT) ? param->open.handle : param->srv_open.handle,
                     bda2str((event == ESP_SPP_OPEN_EVT) ? param->open.rem_bda : param->srv_open.rem_bda,
                            bda_str, sizeof(bda_str)));
            // Save the connected device address for reconnection
            if (event == ESP_SPP_OPEN_EVT) {
                memcpy(s_last_connected_bda, param->open.rem_bda, 6);
            } else {
                memcpy(s_last_connected_bda, param->srv_open.rem_bda, 6);
            }
            s_spp_handle = (event == ESP_SPP_OPEN_EVT) ? param->open.handle : param->srv_open.handle;
            bool is_reconnection = s_trying_reconnect;  // 先保存是否是重连
            if (is_reconnection) {
                // If we were reconnecting, go back to game ready state
                bool was_in_game = s_was_in_game;  // 先保存状态
                s_trying_reconnect = false;
                s_reconnect_attempts = 0;
                s_just_reconnected = true;  // 设置刚重连标志
                s_just_reconnected_time = esp_timer_get_time();  // 记录时间
                set_state(was_in_game ? BT_CHESS_STATE_GAME_READY : BT_CHESS_STATE_CONNECTED);
                s_was_in_game = false;

                // 主机：发送hello和同步
                if (s_role == BT_CHESS_ROLE_HOST) {
                    s_local_color = COLOR_WHITE;
                    // 从保存的状态恢复回合！如果保存的状态当前是白方走，那么主机（白方）的回合是 true
                    s_is_my_turn = (s_saved_game_state.current_turn == COLOR_WHITE);
                    // 发送同步（先发送同步，这样从机可以先恢复状态）
                    bt_chess_send_game_sync(&s_saved_game_state);
                    // 发送待确认的移动
                    bt_chess_resend_pending_moves();
                    // 主机在发送完同步后，也清除重连标志并进入 PLAYING
                    s_just_reconnected = false;
                    if (s_state == BT_CHESS_STATE_GAME_READY) {
                        set_state(BT_CHESS_STATE_PLAYING);
                    }
                    // 重连时不调用 invite_callback，避免触发屏幕切换
                } else {
                    // 从设备：请求同步棋盘
                    s_local_color = COLOR_BLACK;
                    // 先设置回合，等收到同步后再确认
                    s_is_my_turn = (s_saved_game_state.current_turn == COLOR_BLACK);
                    // 先设置状态为 playing，让UI可以正常响应
                    if (s_state == BT_CHESS_STATE_GAME_READY) {
                        set_state(BT_CHESS_STATE_PLAYING);
                    }
                    bt_chess_request_game_sync();
                    // 发送待确认的移动
                    bt_chess_resend_pending_moves();
                }
            } else {
                set_state(BT_CHESS_STATE_CONNECTED);
            }
            if (s_connected_callback) {
                s_connected_callback();
            }
            if (s_role == BT_CHESS_ROLE_HOST && !is_reconnection) {
                // 这里只处理新连接，重连的已经在上面处理了
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
            // Check if we were in a game - if yes, try to reconnect
            if (s_state >= BT_CHESS_STATE_GAME_READY && s_state <= BT_CHESS_STATE_GAME_OVER) {
                s_was_in_game = true;
                s_reconnect_attempts = 0;
                set_state(BT_CHESS_STATE_RECONNECTING);
                s_trying_reconnect = true;

                if (s_role == BT_CHESS_ROLE_HOST) {
                    // 主设备：重新启动服务器，等待从设备连接
                    ESP_LOGI(BT_CHESS_TAG, "Host: Restarting server, waiting for guest to reconnect");
                    if (s_spp_server_handle != 0) {
                        esp_spp_stop_srv();
                        s_spp_server_handle = 0;
                    }
                    esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, BT_CHESS_SERVER_NAME);
                } else {
                    // 从设备：先不主动连，等用户按 A 再连
                    ESP_LOGI(BT_CHESS_TAG, "Guest: Disconnected, waiting for user to press A to reconnect");
                    // 确保从机不在扫描或广播
                    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
                    // 保持在 RECONNECTING 状态但不主动连接
                }
            } else {
                s_was_in_game = false;
                s_trying_reconnect = false;
                set_state(BT_CHESS_STATE_IDLE);
            }
            if (s_disconnected_callback) {
                s_disconnected_callback();
            }
            break;
        case ESP_SPP_START_EVT:
            ESP_LOGI(BT_CHESS_TAG, "SPP server started, handle: %"PRIu32, param->start.handle);
            s_spp_server_handle = param->start.handle;
            char device_name[BT_CHESS_NAME_LEN];
            generate_device_name(device_name, sizeof(device_name));
            ESP_LOGI(BT_CHESS_TAG, "Setting device name for advertising: %s", device_name);
            esp_bt_gap_set_device_name(device_name);
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            // If we were trying to reconnect, stay in reconnecting state (waiting for client)
            if (!s_trying_reconnect) {
                set_state(BT_CHESS_STATE_ADVERTISING);
            }
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
            const char* current_prefix = get_current_prefix();
            ESP_LOGI(BT_CHESS_TAG, "Looking for devices with prefix: %s", current_prefix);

            for (int i = 0; i < param->disc_res.num_prop; i++) {
                if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_EIR &&
                    get_name_from_eir(param->disc_res.prop[i].val, bdname, &bdname_len)) {
                    ESP_LOGI(BT_CHESS_TAG, "Found device: %s", bdname);
                    char hash[7];
                    if (is_chess_device(bdname, hash, sizeof(hash))) {
                        ESP_LOGI(BT_CHESS_TAG, "Found matching game device: %s (hash: %s)", bdname, hash);
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
                            strncpy(dev->name, bdname, BT_CHESS_NAME_LEN - 1);
                            dev->name[BT_CHESS_NAME_LEN - 1] = '\0';
                            strncpy(dev->hash, hash, 6);
                            dev->hash[6] = '\0';
                            int current_count = s_found_device_count;
                            s_found_device_count++;
                            _lock_release(&s_bluetooth_lock);

                            ESP_LOGI(BT_CHESS_TAG, "Added device to list, total: %d", current_count + 1);
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
            ESP_LOGI(BT_CHESS_TAG, "Discovery state changed: %d", param->disc_st_chg.state);
            _lock_acquire(&s_bluetooth_lock);
            int count = s_found_device_count;
            _lock_release(&s_bluetooth_lock);
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED && s_state == BT_CHESS_STATE_SCANNING) {
                if (s_device_list_complete_callback) {
                    s_device_list_complete_callback(count);
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
    _lock_init(&s_bluetooth_lock);
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

    // Reset pending moves
    s_pending_count = 0;
    s_next_sequence_num = 1;
    s_last_acked_sequence = 0;
    s_sync_requested = false;
    // 正确初始化 saved game state
    chess_game_init(&s_saved_game_state);

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

void bt_chess_set_game_type(BluetoothGameType game_type)
{
    ESP_LOGI(BT_CHESS_TAG, "Setting game type: %d", game_type);
    s_game_type = game_type;

    // 如果蓝牙已经初始化，更新设备名称
    if (s_initialized) {
        char device_name[BT_CHESS_NAME_LEN];
        generate_device_name(device_name, sizeof(device_name));
        esp_bt_gap_set_device_name(device_name);
        ESP_LOGI(BT_CHESS_TAG, "Updated device name to: %s", device_name);
    }
}

BluetoothGameType bt_chess_get_game_type(void)
{
    return s_game_type;
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
    // 简单直接返回，避免任何可能导致卡死的代码
    return s_is_my_turn;
}

// 调试函数：获取内部状态
void bt_chess_debug_dump(void)
{
    ESP_LOGI(BT_CHESS_TAG, "DEBUG: state=%d, role=%d, local_color=%d, is_my_turn=%d",
             s_state, s_role, s_local_color, s_is_my_turn);
    ESP_LOGI(BT_CHESS_TAG, "DEBUG: spp_handle=%"PRIu32", spp_server_handle=%"PRIu32,
             s_spp_handle, s_spp_server_handle);
    ESP_LOGI(BT_CHESS_TAG, "DEBUG: trying_reconnect=%d, was_in_game=%d, just_reconnected=%d",
             s_trying_reconnect, s_was_in_game, s_just_reconnected);
}

bool bt_chess_start_hosting(void)
{
    ESP_LOGI(BT_CHESS_TAG, "Starting hosting for game type: %d", s_game_type);
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
    const char* server_name = get_current_server_name();
    esp_err_t ret = esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, server_name);
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
    _lock_acquire(&s_bluetooth_lock);
    s_role = BT_CHESS_ROLE_GUEST;
    s_found_device_count = 0;
    _lock_release(&s_bluetooth_lock);
    char device_name[BT_CHESS_NAME_LEN];
    generate_device_name(device_name, sizeof(device_name));
    esp_bt_gap_set_device_name(device_name);
    set_state(BT_CHESS_STATE_SCANNING);
    esp_err_t ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 30, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_CHESS_TAG, "Failed to start discovery: %s", esp_err_to_name(ret));
        set_state(BT_CHESS_STATE_IDLE);
        _lock_acquire(&s_bluetooth_lock);
        s_role = BT_CHESS_ROLE_NONE;
        _lock_release(&s_bluetooth_lock);
        return false;
    }
    return true;
}

void bt_chess_stop_scanning(void)
{
    ESP_LOGI(BT_CHESS_TAG, "Stopping scan");
    if (s_state == BT_CHESS_STATE_SCANNING) {
        ESP_LOGI(BT_CHESS_TAG, "Cancelling discovery...");
        esp_bt_gap_cancel_discovery();
        ESP_LOGI(BT_CHESS_TAG, "Setting state to idle...");
        set_state(BT_CHESS_STATE_IDLE);
        ESP_LOGI(BT_CHESS_TAG, "Acquiring bluetooth lock...");
        _lock_acquire(&s_bluetooth_lock);
        ESP_LOGI(BT_CHESS_TAG, "Updating role...");
        s_role = BT_CHESS_ROLE_NONE;
        ESP_LOGI(BT_CHESS_TAG, "Releasing bluetooth lock...");
        _lock_release(&s_bluetooth_lock);
        ESP_LOGI(BT_CHESS_TAG, "Scan stopped successfully");
    }
}

int bt_chess_get_found_device_count(void)
{
    _lock_acquire(&s_bluetooth_lock);
    int count = s_found_device_count;
    _lock_release(&s_bluetooth_lock);
    return count;
}

const FoundBluetoothDevice* bt_chess_get_found_device(int index)
{
    _lock_acquire(&s_bluetooth_lock);
    const FoundBluetoothDevice* device = NULL;
    if (index >= 0 && index < s_found_device_count) {
        device = &s_found_devices[index];
    }
    _lock_release(&s_bluetooth_lock);
    return device;
}

int bt_chess_copy_found_devices(FoundBluetoothDevice* buffer, size_t max_count)
{
    _lock_acquire(&s_bluetooth_lock);
    int count = (s_found_device_count < max_count) ? s_found_device_count : max_count;
    if (buffer && count > 0) {
        memcpy(buffer, s_found_devices, count * sizeof(FoundBluetoothDevice));
    }
    _lock_release(&s_bluetooth_lock);
    return count;
}

bool bt_chess_connect_to_device(int index)
{
    ESP_LOGI(BT_CHESS_TAG, "Connecting to device index: %d", index);
    if (!s_initialized) {
        ESP_LOGE(BT_CHESS_TAG, "Bluetooth not initialized");
        return false;
    }
    _lock_acquire(&s_bluetooth_lock);
    if (index < 0 || index >= s_found_device_count) {
        _lock_release(&s_bluetooth_lock);
        ESP_LOGE(BT_CHESS_TAG, "Invalid device index");
        return false;
    }
    if (s_state != BT_CHESS_STATE_SCANNING) {
        _lock_release(&s_bluetooth_lock);
        ESP_LOGW(BT_CHESS_TAG, "Cannot connect in state: %d", s_state);
        return false;
    }
    memcpy(s_target_bda, s_found_devices[index].bda, 6);
    _lock_release(&s_bluetooth_lock);
    esp_bt_gap_cancel_discovery();
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
    // 发送带有序列号的移动消息
    uint8_t msg[11];
    msg[0] = 10;  // 消息长度
    msg[1] = BT_CHESS_MSG_MOVE;
    // 添加序列号
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
        // 保存到待确认队列
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
    // Reset reconnection state
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

void bt_chess_set_sync_callback(bt_chess_sync_received_callback_t callback)
{
    s_sync_callback = callback;
}

bool bt_chess_try_reconnect(void)
{
    if (!s_initialized) {
        return false;
    }
    if (!s_trying_reconnect) {
        return false;
    }
    ESP_LOGI(BT_CHESS_TAG, "Manual reconnect attempt to device: %s",
             bda2str(s_last_connected_bda, (char[18]){0}, 18));
    s_reconnect_attempts++;
    if (s_role == BT_CHESS_ROLE_HOST) {
        // Host just stays in advertising mode - wait for guest to reconnect
        return true;
    } else {
        // Guest tries to connect
        if (s_spp_server_handle != 0) {
            // If we have a server running, stop it
            esp_spp_stop_srv();
            s_spp_server_handle = 0;
        }
        // Make sure we're not advertising if we're guest
        esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        memcpy(s_target_bda, s_last_connected_bda, 6);
        set_state(BT_CHESS_STATE_CONNECTING);
        esp_err_t ret = esp_spp_start_discovery(s_last_connected_bda);
        return (ret == ESP_OK);
    }
}

bool bt_chess_was_in_game(void)
{
    return s_was_in_game || s_trying_reconnect;
}

// 保存游戏状态，用于同步
void bt_chess_save_game_state(const ChessGame* game)
{
    if (game != NULL) {
        memcpy(&s_saved_game_state, game, sizeof(ChessGame));
    }
}

// 发送完整的游戏状态同步
bool bt_chess_send_game_sync(const ChessGame* game)
{
    ESP_LOGI(BT_CHESS_TAG, "Sending full game sync");
    if (s_spp_handle == 0 || game == NULL) {
        return false;
    }

    // 序列化格式：
    // [棋盘 128 字节][最后一步 16 字节][状态字段]
    uint8_t sync_msg[256];
    size_t msg_len = 0;
    sync_msg[msg_len++] = 0;  // 长度占位
    sync_msg[msg_len++] = BT_CHESS_MSG_SYNC;

    // 发送棋盘 (board[8][8]，每个格子 type + color)
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            ChessPiece piece = game->board[y][x];
            sync_msg[msg_len++] = piece.type;
            sync_msg[msg_len++] = piece.color;
        }
    }

    // 发送最后一步走法 (16 字节)
    sync_msg[msg_len++] = game->last_move.from_x;
    sync_msg[msg_len++] = game->last_move.from_y;
    sync_msg[msg_len++] = game->last_move.to_x;
    sync_msg[msg_len++] = game->last_move.to_y;
    sync_msg[msg_len++] = game->last_move.captured_piece.type;
    sync_msg[msg_len++] = game->last_move.captured_piece.color;
    sync_msg[msg_len++] = game->last_move.is_castle_kingside;
    sync_msg[msg_len++] = game->last_move.is_castle_queenside;
    sync_msg[msg_len++] = game->last_move.is_en_passant;
    sync_msg[msg_len++] = game->last_move.is_promotion;
    sync_msg[msg_len++] = game->last_move.promotion_type;

    // 发送游戏状态
    sync_msg[msg_len++] = game->current_turn;  // 虽然基于步数判断，但也发送用于验证
    sync_msg[msg_len++] = game->white_castle_kingside;
    sync_msg[msg_len++] = game->white_castle_queenside;
    sync_msg[msg_len++] = game->black_castle_kingside;
    sync_msg[msg_len++] = game->black_castle_queenside;
    sync_msg[msg_len++] = game->en_passant_x;
    sync_msg[msg_len++] = game->en_passant_y;
    sync_msg[msg_len++] = game->fullmove_number;  // 关键：用于判断谁的回合
    sync_msg[msg_len++] = game->halfmove_clock;
    sync_msg[msg_len++] = game->game_over;
    sync_msg[msg_len++] = game->winner;

    // 更新长度
    sync_msg[0] = msg_len - 1;

    ESP_LOGI(BT_CHESS_TAG, "Sync sent: fullmove=%d, turn=%s",
             game->fullmove_number,
             game->current_turn == COLOR_WHITE ? "white" : "black");

    return send_data(sync_msg, msg_len);
}

// 请求同步棋盘
void bt_chess_request_game_sync(void)
{
    ESP_LOGI(BT_CHESS_TAG, "Requesting game sync");
    if (s_spp_handle == 0) {
        return;
    }
    uint8_t msg[2];
    msg[0] = 1;
    msg[1] = BT_CHESS_MSG_SYNC_REQUEST;
    send_data(msg, 2);
    s_sync_requested = true;
}

// 处理同步数据
void bt_chess_process_game_sync(const uint8_t* data, size_t len)
{
    ESP_LOGI(BT_CHESS_TAG, "Processing game sync, len=%d", len);
    // 兼容旧格式和新格式：
    // 旧格式: 128 + 7 = 135 字节
    // 新格式: 128 + 11 + 12 = 151 字节
    if (len < 135) {
        ESP_LOGW(BT_CHESS_TAG, "Sync data too short, need at least 135 bytes");
        return;
    }

    // 反序列化
    ChessGame temp_game;
    chess_game_init(&temp_game);

    size_t offset = 0;
    // 恢复棋盘 (board[8][8])
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            temp_game.board[y][x].type = data[offset++];
            temp_game.board[y][x].color = data[offset++];
        }
    }

    // 检查是否是新格式 (150或151都接受，兼容不同版本)
    bool is_new_format = (len >= 150);
    if (is_new_format) {
        // 新格式：恢复最后一步走法 (11 字节)
        temp_game.last_move.from_x = data[offset++];
        temp_game.last_move.from_y = data[offset++];
        temp_game.last_move.to_x = data[offset++];
        temp_game.last_move.to_y = data[offset++];
        temp_game.last_move.captured_piece.type = data[offset++];
        temp_game.last_move.captured_piece.color = data[offset++];
        temp_game.last_move.is_castle_kingside = data[offset++];
        temp_game.last_move.is_castle_queenside = data[offset++];
        temp_game.last_move.is_en_passant = data[offset++];
        temp_game.last_move.is_promotion = data[offset++];
        temp_game.last_move.promotion_type = data[offset++];
    }

    // 恢复状态 (新格式: 12 字节, 旧格式: 7 字节)
    temp_game.current_turn = data[offset++];
    temp_game.white_castle_kingside = data[offset++];
    temp_game.white_castle_queenside = data[offset++];
    temp_game.black_castle_kingside = data[offset++];
    temp_game.black_castle_queenside = data[offset++];

    if (is_new_format) {
        // 新格式：更多状态
        temp_game.en_passant_x = data[offset++];
        temp_game.en_passant_y = data[offset++];
    }
    temp_game.fullmove_number = data[offset++];
    temp_game.halfmove_clock = data[offset++];

    if (is_new_format && len >= 151) {
        // 新格式：游戏结束状态
        temp_game.game_over = data[offset++];
        temp_game.winner = data[offset++];
    }

    // 安全检查：fullmove_number 至少为 1
    if (temp_game.fullmove_number < 1) {
        ESP_LOGW(BT_CHESS_TAG, "Fixing invalid fullmove_number=%d", temp_game.fullmove_number);
        temp_game.fullmove_number = 1;
    }

    // 基于步数判断谁的回合！
    // 规则：白棋先走，fullmove_number 在黑棋走完后加 1
    ChessColor expected_turn;

    // 检查最后一步是否有效
    bool last_move_valid = (temp_game.last_move.from_x >= 0 && temp_game.last_move.from_x < 8 &&
                             temp_game.last_move.from_y >= 0 && temp_game.last_move.from_y < 8 &&
                             (temp_game.last_move.from_x != temp_game.last_move.to_x ||
                              temp_game.last_move.from_y != temp_game.last_move.to_y));

    if (last_move_valid) {
        // 从棋盘上看看最后走的位置是什么颜色的棋子
        ChessPiece last_moved_piece = temp_game.board[temp_game.last_move.to_x][temp_game.last_move.to_y];
        if (last_moved_piece.type != PIECE_NONE) {
            // 上一个是这个颜色走的，现在换另一个颜色
            expected_turn = (last_moved_piece.color == COLOR_WHITE) ? COLOR_BLACK : COLOR_WHITE;
        } else {
            // 基于 fullmove_number 判断：奇数是白棋，偶数是黑棋
            expected_turn = (temp_game.fullmove_number % 2 == 1) ? COLOR_WHITE : COLOR_BLACK;
        }
    } else {
        // 基于 fullmove_number 判断：奇数是白棋，偶数是黑棋
        expected_turn = (temp_game.fullmove_number % 2 == 1) ? COLOR_WHITE : COLOR_BLACK;
    }

    // 用计算出来的回合覆盖接收到的 current_turn，确保一致性
    temp_game.current_turn = expected_turn;

    // 保存并通知 UI 更新
    memcpy(&s_saved_game_state, &temp_game, sizeof(ChessGame));

    // 更新自己的回合状态！
    // 主机 = 白方，从机 = 黑方
    // 基于 expected_turn 判断是否是自己的回合
    s_is_my_turn = (s_role == BT_CHESS_ROLE_HOST) ?
                    (expected_turn == COLOR_WHITE) :
                    (expected_turn == COLOR_BLACK);

    ESP_LOGI(BT_CHESS_TAG, "Sync complete: fullmove=%d, expected_turn=%s, my_color=%s, my_turn=%s",
             temp_game.fullmove_number,
             expected_turn == COLOR_WHITE ? "white" : "black",
             s_role == BT_CHESS_ROLE_HOST ? "white" : "black",
             s_is_my_turn ? "YES" : "NO");

    // 通知 UI 更新棋盘
    if (s_sync_callback) {
        s_sync_callback(&temp_game);
    }

    s_sync_requested = false;
    s_just_reconnected = false;  // 同步完成，清除重连标志

    // 确保状态正确
    if (s_state == BT_CHESS_STATE_GAME_READY) {
        set_state(BT_CHESS_STATE_PLAYING);
    }
}

// 检查是否有待确认的移动
bool bt_chess_has_pending_moves(void)
{
    return s_pending_count > 0;
}

// 重发待确认的移动
void bt_chess_resend_pending_moves(void)
{
    ESP_LOGI(BT_CHESS_TAG, "Resending %d pending moves", s_pending_count);
    if (s_spp_handle == 0 || s_pending_count == 0) {
        return;
    }

    for (int i = 0; i < s_pending_count; i++) {
        if (!s_pending_moves[i].acknowledged) {
            PendingMove* pm = &s_pending_moves[i];
            uint8_t msg[11];
            msg[0] = 10;
            msg[1] = BT_CHESS_MSG_MOVE;
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
            ESP_LOGI(BT_CHESS_TAG, "Resent move seq=%"PRIu32, pm->sequence_num);
        }
    }
}
