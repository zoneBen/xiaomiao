#ifndef JUNQI_UI_H
#define JUNQI_UI_H

#include "junqi.h"
#include "tft_display.h"
#include "ui_lock.h"

void junqi_ui_init(void);
void junqi_ui_deinit(void);
void junqi_ui_switch_to_game(void);
void junqi_ui_handle_key(int key);
void junqi_ui_render(void);
void junqi_ui_update(void);
bool junqi_ui_should_exit(void);

#endif // JUNQI_UI_H
