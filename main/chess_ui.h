#ifndef CHESS_UI_H
#define CHESS_UI_H

#include "lvgl.h"
#include "chess.h"

typedef enum {
    SCREEN_MAIN_MENU,
    SCREEN_NETWORK_SETUP,
    SCREEN_LOBBY,
    SCREEN_GAME
} ChessScreen;

void chess_ui_init(void);
void chess_ui_deinit(void);
void chess_ui_switch_screen(ChessScreen screen);
void chess_ui_update_game(const ChessGame* game);
void chess_ui_set_network_status(const char* status);
void chess_ui_show_invite_dialog(const char* opponent_id);
void chess_ui_hide_invite_dialog(void);
void chess_ui_show_message(const char* message, int duration_ms);

#endif
