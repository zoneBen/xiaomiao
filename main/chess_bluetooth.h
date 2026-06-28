#ifndef CHESS_BLUETOOTH_H
#define CHESS_BLUETOOTH_H

#include "chess.h"
#include <stdbool.h>
#include <stdint.h>

// Bluetooth device name prefix and format
#define BT_CHESS_PREFIX "xc-chess-"
#define BT_CHESS_NAME_LEN 32

// Bluetooth message types (similar to network messages)
typedef enum {
    BT_CHESS_MSG_HELLO = 1,
    BT_CHESS_MSG_INVITE = 2,
    BT_CHESS_MSG_ACCEPT = 3,
    BT_CHESS_MSG_REJECT = 4,
    BT_CHESS_MSG_MOVE = 5,
    BT_CHESS_MSG_SYNC = 6,
    BT_CHESS_MSG_GAME_OVER = 7,
    BT_CHESS_MSG_RESIGN = 8,
    BT_CHESS_MSG_CHAT = 9
} BluetoothChessMessageType;

// Bluetooth connection state
typedef enum {
    BT_CHESS_STATE_IDLE,
    BT_CHESS_STATE_ADVERTISING,     // Hosting a game (acceptor)
    BT_CHESS_STATE_SCANNING,        // Looking for games (initiator)
    BT_CHESS_STATE_CONNECTING,
    BT_CHESS_STATE_CONNECTED,
    BT_CHESS_STATE_GAME_READY,
    BT_CHESS_STATE_PLAYING,
    BT_CHESS_STATE_GAME_OVER
} BluetoothChessState;

// Role in the Bluetooth connection
typedef enum {
    BT_CHESS_ROLE_NONE,
    BT_CHESS_ROLE_HOST,            // Acceptor (creates game)
    BT_CHESS_ROLE_GUEST            // Initiator (joins game)
} BluetoothChessRole;

// Found device info
typedef struct {
    uint8_t bda[6];                 // Bluetooth device address
    char name[BT_CHESS_NAME_LEN];    // Device name
    char hash[7];                   // 6-character hash from name
} FoundBluetoothDevice;

// Callbacks for Bluetooth events
typedef void (*bt_chess_state_callback_t)(BluetoothChessState state);
typedef void (*bt_chess_device_found_callback_t)(const FoundBluetoothDevice* device, int index);
typedef void (*bt_chess_device_list_complete_callback_t)(int count);
typedef void (*bt_chess_connected_callback_t)(void);
typedef void (*bt_chess_disconnected_callback_t)(void);
typedef void (*bt_chess_invite_received_callback_t)(void);
typedef void (*bt_chess_invite_accepted_callback_t)(ChessColor local_color);
typedef void (*bt_chess_move_received_callback_t)(int from_x, int from_y, int to_x, int to_y, ChessPieceType promotion);
typedef void (*bt_chess_game_over_callback_t)(ChessColor winner);
typedef void (*bt_chess_chat_received_callback_t)(const char* message);

// Initialize Bluetooth chess module
void bt_chess_init(void);

// Deinitialize Bluetooth chess module
void bt_chess_deinit(void);

// Get current state
BluetoothChessState bt_chess_get_state(void);

// Get current role
BluetoothChessRole bt_chess_get_role(void);

// Get local player color (valid when in game)
ChessColor bt_chess_get_local_color(void);

// Check if it's local player's turn
bool bt_chess_is_my_turn(void);

// --- Host (Create Game) functions ---

// Start advertising as a host (create game)
bool bt_chess_start_hosting(void);

// Stop advertising
void bt_chess_stop_hosting(void);

// Accept an incoming connection/invite
bool bt_chess_accept_connection(void);

// Reject an incoming connection/invite
void bt_chess_reject_connection(void);

// --- Guest (Join Game) functions ---

// Start scanning for available chess games
bool bt_chess_start_scanning(void);

// Stop scanning
void bt_chess_stop_scanning(void);

// Get number of found devices
int bt_chess_get_found_device_count(void);

// Get a found device by index
const FoundBluetoothDevice* bt_chess_get_found_device(int index);

// Connect to a found device
bool bt_chess_connect_to_device(int index);

// Connect to a device by hash (6 characters)
bool bt_chess_connect_to_hash(const char* hash);

// --- Game functions (both roles) ---

// Send a chess move
bool bt_chess_send_move(int from_x, int from_y, int to_x, int to_y, ChessPieceType promotion);

// Send game state sync
bool bt_chess_send_sync(const ChessGame* game);

// Send resignation
bool bt_chess_send_resign(void);

// Send chat message
bool bt_chess_send_chat(const char* message);

// Send game over
bool bt_chess_send_game_over(ChessColor winner);

// Disconnect current connection
void bt_chess_disconnect(void);

// --- Callback registration ---

void bt_chess_set_state_callback(bt_chess_state_callback_t callback);
void bt_chess_set_device_found_callback(bt_chess_device_found_callback_t callback);
void bt_chess_set_device_list_complete_callback(bt_chess_device_list_complete_callback_t callback);
void bt_chess_set_connected_callback(bt_chess_connected_callback_t callback);
void bt_chess_set_disconnected_callback(bt_chess_disconnected_callback_t callback);
void bt_chess_set_invite_callback(bt_chess_invite_received_callback_t callback);
void bt_chess_set_invite_accepted_callback(bt_chess_invite_accepted_callback_t callback);
void bt_chess_set_move_callback(bt_chess_move_received_callback_t callback);
void bt_chess_set_game_over_callback(bt_chess_game_over_callback_t callback);
void bt_chess_set_chat_callback(bt_chess_chat_received_callback_t callback);

#endif
