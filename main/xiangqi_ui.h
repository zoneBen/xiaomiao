#ifndef XIANGQI_UI_H
#define XIANGQI_UI_H

#include "xiangqi.h"
#include "chess_ui.h"
#include "tft_display.h"
#include "ui_lock.h"

void xiangqi_ui_init(void);
void xiangqi_ui_deinit(void);
void xiangqi_ui_switch_to_game(void);
void xiangqi_ui_handle_key(int key);
void xiangqi_ui_render(void);
void xiangqi_ui_update(void);
bool xiangqi_ui_should_exit(void);

#endif // XIANGQI_UI_H
