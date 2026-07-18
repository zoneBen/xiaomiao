#ifndef GOMOKU_UI_H
#define GOMOKU_UI_H

#include "gomoku.h"
#include "chess_ui.h"
#include "tft_display.h"
#include "ui_lock.h"

void gomoku_ui_init(void);
void gomoku_ui_deinit(void);
void gomoku_ui_switch_to_game(void);
void gomoku_ui_handle_key(int key);
void gomoku_ui_render(void);
void gomoku_ui_update(void);
bool gomoku_ui_should_exit(void);

#endif // GOMOKU_UI_H