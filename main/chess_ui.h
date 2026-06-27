#ifndef CHESS_UI_H
#define CHESS_UI_H

#include "chess.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SCREEN_MAIN_MENU,
    SCREEN_NETWORK_SETUP,
    SCREEN_LOBBY,
    SCREEN_GAME
} ChessScreen;

// Key codes
typedef enum {
    KEY_NONE,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_A,
    KEY_B
} KeyCode;

void chess_ui_init(void);
void chess_ui_deinit(void);
void chess_ui_switch_screen(ChessScreen screen);
void chess_ui_update_game(const ChessGame* game);
void chess_ui_set_network_status(const char* status);
void chess_ui_show_invite_dialog(const char* opponent_id);
void chess_ui_hide_invite_dialog(void);
void chess_ui_show_message(const char* message, int duration_ms);
void chess_ui_handle_key(KeyCode key);
void chess_ui_update(void);
void chess_ui_render(void);

#endif
