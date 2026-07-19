#include "gomoku_ui.h"
#include "sound.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "chess_bluetooth.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "GomokuUI";

typedef struct {
    GomokuScreen current_screen;
    GomokuScreen previous_screen;
    int menu_index;
    int menu_count;

    GomokuGame game;
    int cursor_x;
    int cursor_y;
    bool game_over;

    bool bluetooth_initialized;
    int selected_device_index;
    char bluetooth_status[64];

    bool show_message;
    char message_text[32];
    int message_timer;

    int64_t splash_start_time;
} GomokuUIState;

static GomokuUIState* g_ui = NULL;
static TFT_Sprite g_sprite;
static bool g_sprite_created = false;
static int64_t g_last_render = 0;
static bool s_should_exit = false;

#define GOMOKU_SQUARE_SIZE 10
#define GOMOKU_OFFSET_X 22
#define GOMOKU_OFFSET_Y 8

static const char* main_menu_items[] = {
    "Local Game",
    "Bluetooth Game",
    "Back"
};

static const char* bluetooth_menu_items[] = {
    "Create Game",
    "Join Game",
    "Back"
};

static const char* exit_confirm_items[] = {
    "Confirm Exit",
    "Cancel"
};

void gomoku_ui_init(void)
{
    if (g_ui != NULL) {
        return;
    }

    ESP_LOGI(TAG, "Initializing Gomoku UI");

    g_ui = (GomokuUIState*)calloc(1, sizeof(GomokuUIState));
    if (!g_ui) {
        ESP_LOGE(TAG, "Failed to allocate Gomoku UI");
        return;
    }

    gomoku_game_init(&g_ui->game);
    g_ui->current_screen = GM_SCREEN_SPLASH;
    g_ui->previous_screen = GM_SCREEN_SPLASH;
    g_ui->menu_index = 0;
    g_ui->menu_count = 0;
    g_ui->cursor_x = GOMOKU_BOARD_SIZE / 2;
    g_ui->cursor_y = GOMOKU_BOARD_SIZE / 2;
    g_ui->game_over = false;
    g_ui->bluetooth_initialized = false;
    g_ui->selected_device_index = -1;
    strcpy(g_ui->bluetooth_status, "Bluetooth: Disabled");
    g_ui->splash_start_time = esp_timer_get_time();

    if (tft_sprite_create(&g_sprite, TFT_WIDTH, TFT_HEIGHT) == ESP_OK) {
        g_sprite_created = true;
    }
}

void gomoku_ui_deinit(void)
{
    if (g_ui != NULL) {
        if (g_sprite_created) {
            tft_sprite_delete(&g_sprite);
            g_sprite_created = false;
        }
        free(g_ui);
        g_ui = NULL;
    }
}

void gomoku_ui_switch_screen(GomokuScreen screen)
{
    if (!g_ui) return;

    ESP_LOGI(TAG, "Switching to screen %d", screen);

    g_ui->previous_screen = g_ui->current_screen;
    g_ui->current_screen = screen;
    g_ui->menu_index = 0;

    switch (screen) {
        case GM_SCREEN_SPLASH:
            g_ui->menu_count = 0;
            g_ui->splash_start_time = esp_timer_get_time();
            break;
        case GM_SCREEN_MAIN_MENU:
            g_ui->menu_count = 3;
            break;
        case GM_SCREEN_BLUETOOTH_MENU:
            g_ui->menu_count = 3;
            if (!g_ui->bluetooth_initialized) {
                bt_chess_init();
                g_ui->bluetooth_initialized = true;
                strcpy(g_ui->bluetooth_status, "Bluetooth: Ready");
            }
            break;
        case GM_SCREEN_BLUETOOTH_SCAN:
            g_ui->menu_count = 0;
            g_ui->selected_device_index = -1;
            break;
        case GM_SCREEN_BLUETOOTH_WAITING:
            g_ui->menu_count = 0;
            break;
        case GM_SCREEN_GAME:
            gomoku_game_reset(&g_ui->game);
            g_ui->cursor_x = GOMOKU_BOARD_SIZE / 2;
            g_ui->cursor_y = GOMOKU_BOARD_SIZE / 2;
            g_ui->game_over = false;
            break;
        case GM_SCREEN_EXIT_CONFIRM:
            g_ui->menu_count = 2;
            break;
    }
}

void gomoku_ui_switch_to_game(void)
{
    s_should_exit = false;
    if (!g_ui) {
        gomoku_ui_init();
    }
    gomoku_ui_switch_screen(GM_SCREEN_SPLASH);
}

bool gomoku_ui_should_exit(void)
{
    bool result = s_should_exit;
    s_should_exit = false;
    return result;
}

static void render_splash(void)
{
    for (int y = 0; y < TFT_HEIGHT; y++) {
        uint8_t r = 0x10 + (y * 0x20 / TFT_HEIGHT);
        uint8_t g = 0x10 + (y * 0x18 / TFT_HEIGHT);
        uint8_t b = 0x08 + (y * 0x10 / TFT_HEIGHT);
        uint16_t color = RGB_TO_RGB565(r, g, b);
        for (int x = 0; x < TFT_WIDTH; x++) {
            tft_sprite_draw_pixel(&g_sprite, x, y, color);
        }
    }

    const char* title = "GOMOKU";
    int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 2)) / 2;
    int title_y = 35;
    for (int i = 0; title[i]; i++) {
        tft_sprite_draw_char(&g_sprite, title_x + i * 12 - 1, title_y - 1, title[i], 0, 2);
        tft_sprite_draw_char(&g_sprite, title_x + i * 12 + 1, title_y - 1, title[i], 0, 2);
        tft_sprite_draw_char(&g_sprite, title_x + i * 12 - 1, title_y + 1, title[i], 0, 2);
        tft_sprite_draw_char(&g_sprite, title_x + i * 12 + 1, title_y + 1, title[i], 0, 2);
    }
    for (int i = 0; title[i]; i++) {
        tft_sprite_draw_char(&g_sprite, title_x + i * 12, title_y, title[i], TFT_GOLD, 2);
    }

    const char* subtitle = "Five in a Row";
    int subtitle_x = (TFT_WIDTH - tft_sprite_text_width(subtitle, 1)) / 2;
    int text_x = subtitle_x;
    for (const char* p = subtitle; *p; p++) {
        tft_sprite_draw_char(&g_sprite, text_x, 70, *p, TFT_YELLOW, 1);
        text_x += 6;
    }
}

static void render_main_menu(void)
{
    const char* title = "GOMOKU";
    int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 2)) / 2;
    for (int i = 0; title[i]; i++) {
        tft_sprite_draw_char(&g_sprite, title_x + i * 12, 4, title[i], TFT_WHITE, 2);
    }

    for (int i = 0; i < 3; i++) {
        int y = 35 + i * 22;
        bool is_selected = (i == g_ui->menu_index);
        if (is_selected) {
            tft_sprite_fill_rect(&g_sprite, 20, y, 120, 20, TFT_NAVY);
        }
        int text_x = 25;
        const char* text = main_menu_items[i];
        while (*text) {
            tft_sprite_draw_char(&g_sprite, text_x, y + 6, *text, is_selected ? TFT_YELLOW : TFT_WHITE, 1);
            text_x += 6;
            text++;
        }
    }
}

static void render_bluetooth_menu(void)
{
    const char* title = "Bluetooth Game";
    int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 1)) / 2;
    int text_x = title_x;
    for (const char* p = title; *p; p++) {
        tft_sprite_draw_char(&g_sprite, text_x, 5, *p, TFT_WHITE, 1);
        text_x += 6;
    }

    text_x = 10;
    const char* status = g_ui->bluetooth_status;
    for (const char* p = status; *p; p++) {
        tft_sprite_draw_char(&g_sprite, text_x, 20, *p, TFT_LIGHTGREY, 1);
        text_x += 6;
    }

    for (int i = 0; i < 3; i++) {
        int y = 40 + i * 25;
        bool is_selected = (i == g_ui->menu_index);
        if (is_selected) {
            tft_sprite_fill_rect(&g_sprite, 20, y, 120, 20, TFT_NAVY);
        }
        text_x = 25;
        const char* text = bluetooth_menu_items[i];
        while (*text) {
            tft_sprite_draw_char(&g_sprite, text_x, y + 6, *text, is_selected ? TFT_YELLOW : TFT_WHITE, 1);
            text_x += 6;
            text++;
        }
    }
}

static void render_bluetooth_scan(void)
{
    const char* title = "Scan Results";
    int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 1)) / 2;
    int text_x = title_x;
    for (const char* p = title; *p; p++) {
        tft_sprite_draw_char(&g_sprite, text_x, 5, *p, TFT_WHITE, 1);
        text_x += 6;
    }

    FoundBluetoothDevice local_devices[MAX_FOUND_DEVICES];
    int device_count = bt_chess_copy_found_devices(local_devices, MAX_FOUND_DEVICES);

    if (device_count == 0) {
        text_x = 10;
        const char* searching = "Searching...";
        for (const char* p = searching; *p; p++) {
            tft_sprite_draw_char(&g_sprite, text_x, 25, *p, TFT_YELLOW, 1);
            text_x += 6;
        }
    } else {
        int display_count = device_count < 4 ? device_count : 4;
        for (int i = 0; i < display_count; i++) {
            FoundBluetoothDevice* device = &local_devices[i];
            int y = 25 + i * 25;
            bool is_selected = (i == g_ui->selected_device_index);
            if (is_selected) {
                tft_sprite_fill_rect(&g_sprite, 5, y, TFT_WIDTH - 10, 20, TFT_NAVY);
            }
            text_x = 10;
            char display_name[32];
            snprintf(display_name, sizeof(display_name), "%s", device->name);
            const char* name_ptr = display_name;
            while (*name_ptr && text_x < TFT_WIDTH - 10) {
                tft_sprite_draw_char(&g_sprite, text_x, y + 6, *name_ptr, is_selected ? TFT_YELLOW : TFT_WHITE, 1);
                text_x += 6;
                name_ptr++;
            }
        }
    }

    text_x = 10;
    const char* hint = "B: Back";
    for (const char* p = hint; *p; p++) {
        tft_sprite_draw_char(&g_sprite, text_x, TFT_HEIGHT - 12, *p, TFT_LIGHTGREY, 1);
        text_x += 6;
    }
}

static void render_bluetooth_waiting(void)
{
    const char* title = "Waiting...";
    int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 1)) / 2;
    int text_x = title_x;
    for (const char* p = title; *p; p++) {
        tft_sprite_draw_char(&g_sprite, text_x, 40, *p, TFT_WHITE, 1);
        text_x += 6;
    }

    text_x = 10;
    const char* status = g_ui->bluetooth_status;
    for (const char* p = status; *p; p++) {
        tft_sprite_draw_char(&g_sprite, text_x, 60, *p, TFT_YELLOW, 1);
        text_x += 6;
    }

    text_x = 10;
    const char* hint = "B: Cancel";
    for (const char* p = hint; *p; p++) {
        tft_sprite_draw_char(&g_sprite, text_x, TFT_HEIGHT - 12, *p, TFT_LIGHTGREY, 1);
        text_x += 6;
    }
}

static void gomoku_draw_board(void)
{
    tft_sprite_fill_screen(&g_sprite, COLOR_BACKGROUND);
    uint16_t board_color = RGB_TO_RGB565(0xDE, 0xB8, 0x87);
    int board_w = GOMOKU_BOARD_SIZE * GOMOKU_SQUARE_SIZE;
    int board_h = GOMOKU_BOARD_SIZE * GOMOKU_SQUARE_SIZE;
    tft_sprite_fill_rect(&g_sprite, GOMOKU_OFFSET_X, GOMOKU_OFFSET_Y, board_w, board_h, board_color);

    for (int i = 0; i < GOMOKU_BOARD_SIZE; i++) {
        int x = GOMOKU_OFFSET_X + i * GOMOKU_SQUARE_SIZE;
        int y = GOMOKU_OFFSET_Y + i * GOMOKU_SQUARE_SIZE;
        tft_sprite_draw_line(&g_sprite, x, GOMOKU_OFFSET_Y, x, GOMOKU_OFFSET_Y + board_h - 1, TFT_BLACK);
        tft_sprite_draw_line(&g_sprite, GOMOKU_OFFSET_X, y, GOMOKU_OFFSET_X + board_w - 1, y, TFT_BLACK);
    }

    int positions[5][2] = {{3, 3}, {7, 3}, {5, 5}, {3, 7}, {7, 7}};
    for (int i = 0; i < 5; i++) {
        int x = GOMOKU_OFFSET_X + positions[i][0] * GOMOKU_SQUARE_SIZE;
        int y = GOMOKU_OFFSET_Y + positions[i][1] * GOMOKU_SQUARE_SIZE;
        tft_sprite_fill_rect(&g_sprite, x - 1, y - 1, 3, 3, TFT_BLACK);
    }
}

static void gomoku_draw_pieces(void)
{
    for (int y = 0; y < GOMOKU_BOARD_SIZE; y++) {
        for (int x = 0; x < GOMOKU_BOARD_SIZE; x++) {
            GomokuPiece piece = g_ui->game.board[y][x];
            if (piece == GOMOKU_EMPTY) continue;

            int px = GOMOKU_OFFSET_X + x * GOMOKU_SQUARE_SIZE;
            int py = GOMOKU_OFFSET_Y + y * GOMOKU_SQUARE_SIZE;
            int r = GOMOKU_SQUARE_SIZE / 2 - 1;
            for (int dy = -r; dy <= r; dy++) {
                for (int dx = -r; dx <= r; dx++) {
                    if (dx * dx + dy * dy <= r * r) {
                        uint16_t color = (piece == GOMOKU_BLACK) ? TFT_BLACK : TFT_WHITE;
                        tft_sprite_draw_pixel(&g_sprite, px + dx, py + dy, color);
                    }
                }
            }
        }
    }
}

static void gomoku_draw_cursor(void)
{
    int px = GOMOKU_OFFSET_X + g_ui->cursor_x * GOMOKU_SQUARE_SIZE;
    int py = GOMOKU_OFFSET_Y + g_ui->cursor_y * GOMOKU_SQUARE_SIZE;
    int s = GOMOKU_SQUARE_SIZE;
    uint16_t cursor_color = (g_ui->game.current_player == GOMOKU_BLACK) ? TFT_WHITE : TFT_BLACK;
    tft_sprite_fill_rect(&g_sprite, px - 3, py - 3, 3, 3, cursor_color);
    tft_sprite_fill_rect(&g_sprite, px + s - 1, py - 3, 3, 3, cursor_color);
    tft_sprite_fill_rect(&g_sprite, px - 3, py + s - 1, 3, 3, cursor_color);
    tft_sprite_fill_rect(&g_sprite, px + s - 1, py + s - 1, 3, 3, cursor_color);
}

static void gomoku_draw_info(void)
{
    const char* turn_text = (g_ui->game.current_player == GOMOKU_BLACK) ?
                          "Turn: Black" : "Turn: White";
    int x = 5;
    int y = 120;
    for (const char* p = turn_text; *p; p++) {
        tft_sprite_draw_char(&g_sprite, x, y, *p, TFT_LIGHTGREY, 1);
        x += 6;
    }

    const char* hint = "B: Back";
    x = 125;
    for (const char* p = hint; *p; p++) {
        tft_sprite_draw_char(&g_sprite, x, y, *p, TFT_LIGHTGREY, 1);
        x += 6;
    }
}

static void render_game(void)
{
    gomoku_draw_board();
    gomoku_draw_pieces();
    gomoku_draw_cursor();
    gomoku_draw_info();

    if (g_ui->game.game_over) {
        const char* result;
        if (g_ui->game.winner == GOMOKU_BLACK) {
            result = "Black Wins!";
        } else if (g_ui->game.winner == GOMOKU_WHITE) {
            result = "White Wins!";
        } else {
            result = "Draw!";
        }
        int x = (TFT_WIDTH - tft_sprite_text_width(result, 1)) / 2;
        int y = 50;
        tft_sprite_fill_rect(&g_sprite, x - 5, y - 5, tft_sprite_text_width(result, 1) + 10, 18, COLOR_DIALOG);
        tft_sprite_draw_rect(&g_sprite, x - 5, y - 5, tft_sprite_text_width(result, 1) + 10, 18, TFT_WHITE);
        for (const char* p = result; *p; p++) {
            tft_sprite_draw_char(&g_sprite, x, y, *p, TFT_YELLOW, 1);
            x += 6;
        }

        const char* hint = "A/B: Back";
        x = (TFT_WIDTH - tft_sprite_text_width(hint, 1)) / 2;
        y = 70;
        for (const char* p = hint; *p; p++) {
            tft_sprite_draw_char(&g_sprite, x, y, *p, TFT_LIGHTGREY, 1);
            x += 6;
        }
    }

    if (g_ui->show_message) {
        int x = (TFT_WIDTH - tft_sprite_text_width(g_ui->message_text, 1)) / 2;
        int y = 50;
        tft_sprite_fill_rect(&g_sprite, x - 5, y - 5, tft_sprite_text_width(g_ui->message_text, 1) + 10, 18, COLOR_DIALOG);
        for (const char* p = g_ui->message_text; *p; p++) {
            tft_sprite_draw_char(&g_sprite, x, y, *p, TFT_WHITE, 1);
            x += 6;
        }
    }
}

static void render_exit_confirm(void)
{
    render_game();

    tft_sprite_fill_rect(&g_sprite, 20, 30, 120, 70, COLOR_DIALOG);
    tft_sprite_draw_rect(&g_sprite, 20, 30, 120, 70, TFT_WHITE);

    const char* title = "Exit Game?";
    int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 1)) / 2;
    int text_x = title_x;
    for (const char* p = title; *p; p++) {
        tft_sprite_draw_char(&g_sprite, text_x, 45, *p, TFT_WHITE, 1);
        text_x += 6;
    }

    for (int i = 0; i < 2; i++) {
        int y = 65 + i * 20;
        bool is_selected = (i == g_ui->menu_index);
        if (is_selected) {
            tft_sprite_fill_rect(&g_sprite, 35, y, 90, 18, TFT_NAVY);
        }
        text_x = 40;
        const char* text = exit_confirm_items[i];
        while (*text) {
            tft_sprite_draw_char(&g_sprite, text_x, y + 5, *text, is_selected ? TFT_YELLOW : TFT_WHITE, 1);
            text_x += 6;
            text++;
        }
    }
}

static void render_internal(void)
{
    if (!g_ui || !g_sprite_created) return;

    switch (g_ui->current_screen) {
        case GM_SCREEN_SPLASH:
            render_splash();
            break;
        case GM_SCREEN_MAIN_MENU:
            render_main_menu();
            break;
        case GM_SCREEN_BLUETOOTH_MENU:
            render_bluetooth_menu();
            break;
        case GM_SCREEN_BLUETOOTH_SCAN:
            render_bluetooth_scan();
            break;
        case GM_SCREEN_BLUETOOTH_WAITING:
            render_bluetooth_waiting();
            break;
        case GM_SCREEN_GAME:
            render_game();
            break;
        case GM_SCREEN_EXIT_CONFIRM:
            render_exit_confirm();
            break;
    }

    tft_sprite_push(&g_sprite, 0, 0);
}

void gomoku_ui_render_locked(void)
{
    if (!g_ui || !g_sprite_created) return;

    int64_t now = esp_timer_get_time();
    if (now - g_last_render < 33000) return;
    g_last_render = now;

    render_internal();
}

void gomoku_ui_render(void)
{
    if (!g_ui || !g_sprite_created) return;

    ui_lock_acquire();
    gomoku_ui_render_locked();
    ui_lock_release();
}

void gomoku_ui_handle_key(int key)
{
    if (!g_ui) return;

    ui_lock_acquire();

    switch (g_ui->current_screen) {
        case GM_SCREEN_SPLASH:
            sound_play(SOUND_KEY_CLICK);
            gomoku_ui_switch_screen(GM_SCREEN_MAIN_MENU);
            break;

        case GM_SCREEN_MAIN_MENU:
            if (key == KEY_UP) {
                if (g_ui->menu_index > 0) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->menu_index--;
                }
            } else if (key == KEY_DOWN) {
                if (g_ui->menu_index < g_ui->menu_count - 1) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->menu_index++;
                }
            } else if (key == KEY_A) {
                sound_play(SOUND_MENU_SELECT);
                if (g_ui->menu_index == 0) {
                    sound_play(SOUND_GAME_START);
                    gomoku_ui_switch_screen(GM_SCREEN_GAME);
                } else if (g_ui->menu_index == 1) {
                    gomoku_ui_switch_screen(GM_SCREEN_BLUETOOTH_MENU);
                } else if (g_ui->menu_index == 2) {
                    s_should_exit = true;
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                s_should_exit = true;
            }
            break;

        case GM_SCREEN_BLUETOOTH_MENU:
            if (key == KEY_UP) {
                if (g_ui->menu_index > 0) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->menu_index--;
                }
            } else if (key == KEY_DOWN) {
                if (g_ui->menu_index < 2) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->menu_index++;
                }
            } else if (key == KEY_A) {
                sound_play(SOUND_MENU_SELECT);
                if (g_ui->menu_index == 0) {
                    if (bt_chess_start_hosting()) {
                        strcpy(g_ui->bluetooth_status, "Waiting for opponent...");
                        gomoku_ui_switch_screen(GM_SCREEN_BLUETOOTH_WAITING);
                    }
                } else if (g_ui->menu_index == 1) {
                    if (bt_chess_start_scanning()) {
                        strcpy(g_ui->bluetooth_status, "Scanning...");
                        gomoku_ui_switch_screen(GM_SCREEN_BLUETOOTH_SCAN);
                    }
                } else {
                    gomoku_ui_switch_screen(GM_SCREEN_MAIN_MENU);
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                gomoku_ui_switch_screen(GM_SCREEN_MAIN_MENU);
            }
            break;

        case GM_SCREEN_BLUETOOTH_SCAN:
            if (key == KEY_UP) {
                if (g_ui->selected_device_index > 0) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->selected_device_index--;
                }
            } else if (key == KEY_DOWN) {
                int device_count = bt_chess_get_found_device_count();
                if (g_ui->selected_device_index < device_count - 1) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->selected_device_index++;
                }
            } else if (key == KEY_A) {
                if (g_ui->selected_device_index >= 0) {
                    sound_play(SOUND_MENU_SELECT);
                    if (bt_chess_connect_to_device(g_ui->selected_device_index)) {
                        strcpy(g_ui->bluetooth_status, "Connecting...");
                        gomoku_ui_switch_screen(GM_SCREEN_BLUETOOTH_WAITING);
                    }
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                bt_chess_stop_scanning();
                gomoku_ui_switch_screen(GM_SCREEN_BLUETOOTH_MENU);
            }
            break;

        case GM_SCREEN_BLUETOOTH_WAITING:
            if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                bt_chess_disconnect();
                bt_chess_stop_hosting();
                bt_chess_stop_scanning();
                gomoku_ui_switch_screen(GM_SCREEN_BLUETOOTH_MENU);
            }
            break;

        case GM_SCREEN_EXIT_CONFIRM:
            if (key == KEY_UP) {
                if (g_ui->menu_index > 0) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->menu_index--;
                }
            } else if (key == KEY_DOWN) {
                if (g_ui->menu_index < g_ui->menu_count - 1) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->menu_index++;
                }
            } else if (key == KEY_A) {
                sound_play(SOUND_MENU_SELECT);
                if (g_ui->menu_index == 0) {
                    gomoku_ui_switch_screen(GM_SCREEN_MAIN_MENU);
                } else {
                    gomoku_ui_switch_screen(g_ui->previous_screen);
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                gomoku_ui_switch_screen(g_ui->previous_screen);
            }
            break;

        case GM_SCREEN_GAME:
            if (g_ui->game.game_over) {
                if (key == KEY_B || key == KEY_A) {
                    sound_play(SOUND_KEY_CLICK);
                    gomoku_ui_switch_screen(GM_SCREEN_MAIN_MENU);
                }
                break;
            }

            if (key == KEY_UP) {
                if (g_ui->cursor_y > 0) {
                    g_ui->cursor_y--;
                    sound_play(SOUND_KEY_CLICK);
                }
            } else if (key == KEY_DOWN) {
                if (g_ui->cursor_y < GOMOKU_BOARD_SIZE - 1) {
                    g_ui->cursor_y++;
                    sound_play(SOUND_KEY_CLICK);
                }
            } else if (key == KEY_LEFT) {
                if (g_ui->cursor_x > 0) {
                    g_ui->cursor_x--;
                    sound_play(SOUND_KEY_CLICK);
                }
            } else if (key == KEY_RIGHT) {
                if (g_ui->cursor_x < GOMOKU_BOARD_SIZE - 1) {
                    g_ui->cursor_x++;
                    sound_play(SOUND_KEY_CLICK);
                }
            } else if (key == KEY_A) {
                if (gomoku_place_piece(&g_ui->game, g_ui->cursor_x, g_ui->cursor_y)) {
                    sound_play(SOUND_MOVE);
                } else {
                    sound_play(SOUND_INVALID_MOVE);
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                gomoku_ui_switch_screen(GM_SCREEN_EXIT_CONFIRM);
            }
            break;
    }

    ui_lock_release();
}

void gomoku_ui_update(void)
{
    if (!g_ui) return;

    if (g_ui->current_screen == GM_SCREEN_SPLASH) {
        int64_t now = esp_timer_get_time();
        if (now - g_ui->splash_start_time > 3000000) {
            gomoku_ui_switch_screen(GM_SCREEN_MAIN_MENU);
        }
    }

    if (g_ui->show_message) {
        if (g_ui->message_timer > 0) {
            g_ui->message_timer--;
        } else {
            g_ui->show_message = false;
        }
    }
}
