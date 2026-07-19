#ifndef BLUETOOTH_GAME_H
#define BLUETOOTH_GAME_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_FOUND_DEVICES 5

// 游戏类型枚举
typedef enum {
    BT_GAME_CHESS = 0,     // 国际象棋
    BT_GAME_XIANGQI,       // 中国象棋
    BT_GAME_GOMOKU,        // 五子棋
    BT_GAME_GO,            // 围棋
    BT_GAME_JUNQI,         // 军棋
} BluetoothGameType;

// 蓝牙设备名称前缀
#define BT_GAME_CHESS_PREFIX "xc-chess-"
#define BT_GAME_XIANGQI_PREFIX "xc-xiangqi-"
#define BT_GAME_GOMOKU_PREFIX "xc-gomoku-"
#define BT_GAME_GO_PREFIX "xc-go-"
#define BT_GAME_JUNQI_PREFIX "xc-junqi-"
#define BT_GAME_NAME_LEN 32

// 蓝牙消息类型
typedef enum {
    BT_GAME_MSG_HELLO = 1,
    BT_GAME_MSG_INVITE = 2,
    BT_GAME_MSG_ACCEPT = 3,
    BT_GAME_MSG_REJECT = 4,
    BT_GAME_MSG_MOVE = 5,
    BT_GAME_MSG_MOVE_ACK = 6,      // 移动确认
    BT_GAME_MSG_SYNC = 7,
    BT_GAME_MSG_SYNC_REQUEST = 8,  // 请求同步棋盘
    BT_GAME_MSG_GAME_OVER = 9,
    BT_GAME_MSG_RESIGN = 10,
    BT_GAME_MSG_CHAT = 11
} BluetoothGameMsgType;

// 未确认的移动记录
#define MAX_PENDING_MOVES 10
typedef struct {
    int from_x, from_y, to_x, to_y;
    int promotion;  // 游戏特定的升级类型
    uint32_t sequence_num;        // 序列号
    bool acknowledged;
} PendingGameMove;

// 蓝牙连接状态
typedef enum {
    BT_GAME_STATE_IDLE,
    BT_GAME_STATE_ADVERTISING,     // 作为主机广播（创建游戏）
    BT_GAME_STATE_SCANNING,        // 扫描游戏（加入游戏）
    BT_GAME_STATE_CONNECTING,
    BT_GAME_STATE_CONNECTED,
    BT_GAME_STATE_GAME_READY,
    BT_GAME_STATE_PLAYING,
    BT_GAME_STATE_GAME_OVER,
    BT_GAME_STATE_RECONNECTING     // 断开后重连中
} BluetoothGameState;

// 蓝牙连接中的角色
typedef enum {
    BT_GAME_ROLE_NONE,
    BT_GAME_ROLE_HOST,             // 主机（创建游戏者）
    BT_GAME_ROLE_GUEST             // 从机（加入游戏者）
} BluetoothGameRole;

// 发现的设备信息
typedef struct {
    uint8_t bda[6];                 // 蓝牙设备地址
    char name[BT_GAME_NAME_LEN];    // 设备名称
    char hash[7];                   // 6字符哈希
} FoundBluetoothDevice;

// 通用游戏状态（用于同步）
typedef struct {
    // 棋盘数据占位符 - 各个游戏可以自定义序列化方式
    uint8_t board_data[256];
    int board_data_len;

    // 通用游戏状态
    int current_turn;           // 当前回合
    int fullmove_number;        // 完整步数
    int halfmove_clock;         // 半回合计数
    bool game_over;
    int winner;                 // 胜利者

    // 最后一步
    int last_from_x, last_from_y;
    int last_to_x, last_to_y;
} GenericGameState;

// 回调函数类型
typedef void (*bt_game_state_callback_t)(BluetoothGameState state);
typedef void (*bt_game_device_found_callback_t)(const FoundBluetoothDevice* device, int index);
typedef void (*bt_game_device_list_complete_callback_t)(int count);
typedef void (*bt_game_connected_callback_t)(void);
typedef void (*bt_game_disconnected_callback_t)(void);
typedef void (*bt_game_invite_received_callback_t)(void);
typedef void (*bt_game_invite_accepted_callback_t)(int local_side);
typedef void (*bt_game_move_received_callback_t)(int from_x, int from_y, int to_x, int to_y, int promotion);
typedef void (*bt_game_game_over_callback_t)(int winner);
typedef void (*bt_game_chat_received_callback_t)(const char* message);
typedef void (*bt_game_sync_received_callback_t)(const GenericGameState* state);

// ============== 主要API函数 ==============

// 设置当前游戏类型（在init之前或之后都可以调用）
void bt_game_set_type(BluetoothGameType game_type);

// 获取当前游戏类型
BluetoothGameType bt_game_get_type(void);

// 初始化蓝牙游戏模块
void bt_game_init(void);

// 清理蓝牙游戏模块
void bt_game_deinit(void);

// 获取当前状态
BluetoothGameState bt_game_get_state(void);

// 获取当前角色
BluetoothGameRole bt_game_get_role(void);

// 获取本方颜色/边（有效时）
int bt_game_get_local_side(void);

// 是否是本方回合
bool bt_game_is_my_turn(void);

// ============== 主机/创建游戏功能 ==============

// 开始作为主机广播（创建游戏）
bool bt_game_start_hosting(void);

// 停止广播
void bt_game_stop_hosting(void);

// 接受连接邀请
bool bt_game_accept_connection(void);

// 拒绝连接邀请
void bt_game_reject_connection(void);

// ============== 从机/加入游戏功能 ==============

// 开始扫描可用游戏
bool bt_game_start_scanning(void);

// 停止扫描
void bt_game_stop_scanning(void);

// 获取发现的设备数量
int bt_game_get_found_device_count(void);

// 获取指定索引的发现设备
const FoundBluetoothDevice* bt_game_get_found_device(int index);

// 安全复制所有发现设备到缓冲区
int bt_game_copy_found_devices(FoundBluetoothDevice* buffer, size_t max_count);

// 连接到选定的设备
bool bt_game_connect_to_device(int index);

// 通过哈希连接到设备（6字符）
bool bt_game_connect_to_hash(const char* hash);

// ============== 游戏消息功能 ==============

// 发送游戏移动
bool bt_game_send_move(int from_x, int from_y, int to_x, int to_y, int promotion);

// 发送游戏状态同步
bool bt_game_send_sync(const GenericGameState* state);

// 发送认输
bool bt_game_send_resign(void);

// 发送聊天消息
bool bt_game_send_chat(const char* message);

// 发送游戏结束
bool bt_game_send_game_over(int winner);

// ============== 连接管理 ==============

// 断开当前连接
void bt_game_disconnect(void);

// 尝试重连到上次配对的设备（仅在之前在游戏中断开后）
bool bt_game_try_reconnect(void);

// 检查之前是否在游戏中
bool bt_game_was_in_game(void);

// ============== 游戏状态同步 ==============

// 保存游戏状态用于同步
void bt_game_save_game_state(const GenericGameState* state);

// 发送完整游戏状态同步（用于重连）
bool bt_game_send_game_sync(const GenericGameState* state);

// 请求对方发送游戏同步
void bt_game_request_game_sync(void);

// 处理收到的同步数据（内部使用）
void bt_game_process_game_sync(const uint8_t* data, size_t len);

// ============== 待确认移动管理 ==============

// 检查是否有待确认的移动
bool bt_game_has_pending_moves(void);

// 重发待确认的移动
void bt_game_resend_pending_moves(void);

// ============== 回调注册 ==============

void bt_game_set_state_callback(bt_game_state_callback_t callback);
void bt_game_set_device_found_callback(bt_game_device_found_callback_t callback);
void bt_game_set_device_list_complete_callback(bt_game_device_list_complete_callback_t callback);
void bt_game_set_connected_callback(bt_game_connected_callback_t callback);
void bt_game_set_disconnected_callback(bt_game_disconnected_callback_t callback);
void bt_game_set_invite_callback(bt_game_invite_received_callback_t callback);
void bt_game_set_invite_accepted_callback(bt_game_invite_accepted_callback_t callback);
void bt_game_set_move_callback(bt_game_move_received_callback_t callback);
void bt_game_set_game_over_callback(bt_game_game_over_callback_t callback);
void bt_game_set_chat_callback(bt_game_chat_received_callback_t callback);
void bt_game_set_sync_callback(bt_game_sync_received_callback_t callback);

// 调试函数：输出内部状态
void bt_game_debug_dump(void);

#endif
