#ifndef CHESS_UI_H
#define CHESS_UI_H

#include "chess.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SCREEN_SPLASH,
    SCREEN_MAIN_MENU,
    SCREEN_BLUETOOTH_MENU,
    SCREEN_BLUETOOTH_SCAN,
    SCREEN_BLUETOOTH_WAITING,
    SCREEN_GAME,
    SCREEN_EXIT_CONFIRM
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
void chess_ui_show_message(const char* message, int duration_ms);
void chess_ui_handle_key(KeyCode key);
void chess_ui_update(void);
void chess_ui_render(void);
void chess_ui_apply_opponent_move(int from_x, int from_y, int to_x, int to_y, ChessPieceType promotion);

// Internal versions - assume ui lock is already held (for UI task only)
void chess_ui_apply_opponent_move_locked(int from_x, int from_y, int to_x, int to_y, ChessPieceType promotion);
void chess_ui_update_game_locked(const ChessGame* game);
void chess_ui_render_locked(void);
void chess_ui_render_locked_force(void);  // Bypasses throttling, forces immediate render

#endif
