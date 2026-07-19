#ifndef JUNQI_UI_H
#define JUNQI_UI_H

#include "junqi.h"
#include "chess_ui.h"
#include "tft_display.h"
#include "ui_lock.h"

typedef enum {
    JQ_SCREEN_SPLASH,
    JQ_SCREEN_MAIN_MENU,
    JQ_SCREEN_BLUETOOTH_MENU,
    JQ_SCREEN_BLUETOOTH_SCAN,
    JQ_SCREEN_BLUETOOTH_WAITING,
    JQ_SCREEN_GAME,
    JQ_SCREEN_EXIT_CONFIRM
} JunqiScreen;

void junqi_ui_init(void);
void junqi_ui_deinit(void);
void junqi_ui_switch_screen(JunqiScreen screen);
void junqi_ui_switch_to_game(void);
void junqi_ui_handle_key(int key);
void junqi_ui_render(void);
void junqi_ui_render_locked(void);
void junqi_ui_update(void);
bool junqi_ui_should_exit(void);

#endif // JUNQI_UI_H
