#ifndef GOMOKU_UI_H
#define GOMOKU_UI_H

#include "gomoku.h"
#include "chess_ui.h"
#include "tft_display.h"
#include "ui_lock.h"

typedef enum {
    GM_SCREEN_SPLASH,
    GM_SCREEN_MAIN_MENU,
    GM_SCREEN_BLUETOOTH_MENU,
    GM_SCREEN_BLUETOOTH_SCAN,
    GM_SCREEN_BLUETOOTH_WAITING,
    GM_SCREEN_GAME,
    GM_SCREEN_EXIT_CONFIRM
} GomokuScreen;

void gomoku_ui_init(void);
void gomoku_ui_deinit(void);
void gomoku_ui_switch_screen(GomokuScreen screen);
void gomoku_ui_switch_to_game(void);
void gomoku_ui_handle_key(int key);
void gomoku_ui_render(void);
void gomoku_ui_render_locked(void);
void gomoku_ui_update(void);
bool gomoku_ui_should_exit(void);

#endif // GOMOKU_UI_H
