#ifndef XIANGQI_UI_H
#define XIANGQI_UI_H

#include "xiangqi.h"
#include "chess_ui.h"
#include "tft_display.h"
#include "ui_lock.h"

// 为中国象棋添加类似国际象棋的屏幕枚举
typedef enum {
    XQ_SCREEN_SPLASH,
    XQ_SCREEN_MAIN_MENU,
    XQ_SCREEN_BLUETOOTH_MENU,
    XQ_SCREEN_BLUETOOTH_SCAN,
    XQ_SCREEN_BLUETOOTH_WAITING,
    XQ_SCREEN_GAME,
    XQ_SCREEN_EXIT_CONFIRM
} XiangqiScreen;

void xiangqi_ui_init(void);
void xiangqi_ui_deinit(void);
void xiangqi_ui_switch_screen(XiangqiScreen screen);
void xiangqi_ui_switch_to_game(void);
void xiangqi_ui_handle_key(int key);
void xiangqi_ui_render(void);
void xiangqi_ui_render_locked(void);
void xiangqi_ui_update(void);
bool xiangqi_ui_should_exit(void);

#endif // XIANGQI_UI_H
