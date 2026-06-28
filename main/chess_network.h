#ifndef CHESS_NETWORK_H
#define CHESS_NETWORK_H

#include "chess.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CHESS_NET_MSG_HELLO = 1,
    CHESS_NET_MSG_SEARCH = 2,
    CHESS_NET_MSG_INVITE = 3,
    CHESS_NET_MSG_ACCEPT = 4,
    CHESS_NET_MSG_REJECT = 5,
    CHESS_NET_MSG_MOVE = 6,
    CHESS_NET_MSG_CHAT = 7,
    CHESS_NET_MSG_RESIGN = 8,
    CHESS_NET_MSG_DRAW_OFFER = 9,
    CHESS_NET_MSG_DRAW_ACCEPT = 10,
    CHESS_NET_MSG_SYNC = 11,
    CHESS_NET_MSG_GAME_OVER = 12
} ChessNetworkMessageType;

typedef enum {
    CHESS_NET_STATE_IDLE,
    CHESS_NET_STATE_SEARCHING,
    CHESS_NET_STATE_WAITING_ACCEPT,
    CHESS_NET_STATE_GAME_READY,
    CHESS_NET_STATE_PLAYING,
    CHESS_NET_STATE_GAME_OVER
} ChessNetworkState;

typedef enum {
    CHESS_NET_PLAYER_LOCAL,
    CHESS_NET_PLAYER_REMOTE
} ChessNetworkPlayerRole;

typedef struct {
    ChessNetworkState state;
    ChessNetworkPlayerRole role;
    char player_id[32];
    char opponent_id[32];
    char game_topic[64];
    ChessColor local_color;
    bool game_ready;
} ChessNetworkContext;

typedef void (*chess_network_state_callback_t)(ChessNetworkState state);
typedef void (*chess_network_invite_callback_t)(const char* opponent_id);
typedef void (*chess_network_move_callback_t)(int from_x, int from_y, int to_x, int to_y, ChessPieceType promotion);
typedef void (*chess_network_chat_callback_t)(const char* message);
typedef void (*chess_network_game_over_callback_t)(ChessColor winner);

void chess_network_init(void);
void chess_network_deinit(void);

void chess_network_set_player_id(const char* id);
const char* chess_network_get_player_id(void);

bool chess_network_start_search(void);
void chess_network_stop_search(void);

bool chess_network_send_invite(const char* opponent_id);
bool chess_network_accept_invite(const char* opponent_id);
bool chess_network_reject_invite(const char* opponent_id);

bool chess_network_send_move(int from_x, int from_y, int to_x, int to_y, ChessPieceType promotion);
bool chess_network_send_chat(const char* message);
bool chess_network_send_resign(void);
bool chess_network_send_game_over(ChessColor winner);
bool chess_network_send_draw_offer(void);
bool chess_network_accept_draw(void);

ChessNetworkState chess_network_get_state(void);
ChessNetworkPlayerRole chess_network_get_role(void);
ChessColor chess_network_get_local_color(void);
bool chess_network_is_my_turn(void);

void chess_network_set_state_callback(chess_network_state_callback_t callback);
void chess_network_set_invite_callback(chess_network_invite_callback_t callback);
void chess_network_set_move_callback(chess_network_move_callback_t callback);
void chess_network_set_chat_callback(chess_network_chat_callback_t callback);
void chess_network_set_game_over_callback(chess_network_game_over_callback_t callback);

void chess_network_sync_game(const ChessGame* game);

#endif
