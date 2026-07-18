#ifndef GO_UI_H
#define GO_UI_H

#include "go.h"
#include "chess_ui.h"
#include "tft_display.h"
#include "ui_lock.h"

void go_ui_init(void);
void go_ui_deinit(void);
void go_ui_switch_to_game(void);
void go_ui_handle_key(int key);
void go_ui_render(void);
void go_ui_update(void);
bool go_ui_should_exit(void);

#endif // GO_UI_H