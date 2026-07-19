#ifndef GO_UI_H
#define GO_UI_H

#include "go.h"
#include "chess_ui.h"
#include "tft_display.h"
#include "ui_lock.h"

typedef enum {
    GO_SCREEN_SPLASH,
    GO_SCREEN_MAIN_MENU,
    GO_SCREEN_BLUETOOTH_MENU,
    GO_SCREEN_BLUETOOTH_SCAN,
    GO_SCREEN_BLUETOOTH_WAITING,
    GO_SCREEN_GAME,
    GO_SCREEN_EXIT_CONFIRM
} GoScreen;

void go_ui_init(void);
void go_ui_deinit(void);
void go_ui_switch_screen(GoScreen screen);
void go_ui_switch_to_game(void);
void go_ui_handle_key(int key);
void go_ui_render(void);
void go_ui_render_locked(void);
void go_ui_update(void);
bool go_ui_should_exit(void);

#endif // GO_UI_H
