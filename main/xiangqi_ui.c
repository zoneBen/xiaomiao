#include "xiangqi_ui.h"
#include "xiangqi_pieces.h"
#include "sound.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "chess_bluetooth.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "XiangqiUI";

// UI状态
typedef struct {
    XiangqiScreen current_screen;
    XiangqiScreen previous_screen;
    int menu_index;
    int menu_count;

    XiangqiGame game;
    int cursor_x;
    int cursor_y;
    int selected_x;
    int selected_y;
    bool has_selection;
    bool game_over;

    // Bluetooth state
    bool bluetooth_initialized;
    int selected_device_index;
    char bluetooth_status[64];

    bool show_message;
    char message_text[32];
    int message_timer;

    int64_t splash_start_time;
} XiangqiUIState;

static XiangqiUIState* g_ui = NULL;
static TFT_Sprite g_sprite;
static bool g_sprite_created = false;
static int64_t g_last_render = 0;
static bool s_should_exit = false;

// 棋盘偏移，让棋盘居中
#define XIANGQI_OFFSET_X 4
#define XIANGQI_OFFSET_Y 0
#define XIANGQI_SQUARE_SIZE 13

// 菜单项
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

void xiangqi_ui_init(void)
{
    if (g_ui != NULL) {
        return;
    }

    ESP_LOGI(TAG, "Initializing Xiangqi UI");

    g_ui = (XiangqiUIState*)calloc(1, sizeof(XiangqiUIState));
    if (!g_ui) {
        ESP_LOGE(TAG, "Failed to allocate Xiangqi UI");
        return;
    }

    xiangqi_game_init(&g_ui->game);
    g_ui->current_screen = XQ_SCREEN_SPLASH;
    g_ui->previous_screen = XQ_SCREEN_SPLASH;
    g_ui->menu_index = 0;
    g_ui->menu_count = 0;
    g_ui->cursor_x = 4;
    g_ui->cursor_y = 9;
    g_ui->selected_x = -1;
    g_ui->selected_y = -1;
    g_ui->has_selection = false;
    g_ui->game_over = false;
    g_ui->bluetooth_initialized = false;
    g_ui->selected_device_index = -1;
    strcpy(g_ui->bluetooth_status, "Bluetooth: Disabled");
    g_ui->splash_start_time = esp_timer_get_time();

    if (tft_sprite_create(&g_sprite, TFT_WIDTH, TFT_HEIGHT) == ESP_OK) {
        g_sprite_created = true;
    }
}

void xiangqi_ui_deinit(void)
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

void xiangqi_ui_switch_screen(XiangqiScreen screen)
{
    if (!g_ui) return;

    ESP_LOGI(TAG, "Switching to screen %d", screen);

    g_ui->previous_screen = g_ui->current_screen;
    g_ui->current_screen = screen;
    g_ui->menu_index = 0;

    switch (screen) {
        case XQ_SCREEN_SPLASH:
            g_ui->menu_count = 0;
            g_ui->splash_start_time = esp_timer_get_time();
            break;
        case XQ_SCREEN_MAIN_MENU:
            g_ui->menu_count = 3;
            break;
        case XQ_SCREEN_BLUETOOTH_MENU:
            g_ui->menu_count = 3;
            if (!g_ui->bluetooth_initialized) {
                bt_chess_init();
                g_ui->bluetooth_initialized = true;
                strcpy(g_ui->bluetooth_status, "Bluetooth: Ready");
            }
            break;
        case XQ_SCREEN_BLUETOOTH_SCAN:
            g_ui->menu_count = 0;
            g_ui->selected_device_index = -1;
            break;
        case XQ_SCREEN_BLUETOOTH_WAITING:
            g_ui->menu_count = 0;
            break;
        case XQ_SCREEN_GAME:
            xiangqi_game_reset(&g_ui->game);
            g_ui->cursor_x = 4;
            g_ui->cursor_y = 9;
            g_ui->selected_x = -1;
            g_ui->selected_y = -1;
            g_ui->has_selection = false;
            g_ui->game_over = false;
            break;
        case XQ_SCREEN_EXIT_CONFIRM:
            g_ui->menu_count = 2;
            break;
    }
}

void xiangqi_ui_switch_to_game(void)
{
    s_should_exit = false;
    if (!g_ui) {
        xiangqi_ui_init();
    }
    xiangqi_ui_switch_screen(XQ_SCREEN_SPLASH);
}

bool xiangqi_ui_should_exit(void)
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

    const char* title = "XIANGQI";
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

    const char* subtitle = "Chinese Chess";
    int subtitle_x = (TFT_WIDTH - tft_sprite_text_width(subtitle, 1)) / 2;
    int text_x = subtitle_x;
    for (const char* p = subtitle; *p; p++) {
        tft_sprite_draw_char(&g_sprite, text_x, 70, *p, TFT_YELLOW, 1);
        text_x += 6;
    }
}

static void render_main_menu(void)
{
    int title_x = (TFT_WIDTH - tft_sprite_text_width("XIANGQI", 2)) / 2;
    for (int i = 0; i < 7; i++) {
        char c = "XIANGQI"[i];
        tft_sprite_draw_char(&g_sprite, title_x + i * 12, 4, c, TFT_WHITE, 2);
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

static void xiangqi_draw_board_lines(void)
{
    tft_sprite_fill_screen(&g_sprite, COLOR_BACKGROUND);
    uint16_t board_bg = RGB_TO_RGB565(0x8B, 0x69, 0x14);
    tft_sprite_fill_rect(&g_sprite, XIANGQI_OFFSET_X, XIANGQI_OFFSET_Y,
                        XIANGQI_BOARD_WIDTH * XIANGQI_SQUARE_SIZE,
                        XIANGQI_BOARD_HEIGHT * XIANGQI_SQUARE_SIZE,
                        board_bg);

    for (int y = 0; y < XIANGQI_BOARD_HEIGHT; y++) {
        int py = XIANGQI_OFFSET_Y + y * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
        int px1 = XIANGQI_OFFSET_X + XIANGQI_SQUARE_SIZE / 2;
        int px2 = XIANGQI_OFFSET_X + (XIANGQI_BOARD_WIDTH - 1) * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
        tft_sprite_draw_line(&g_sprite, px1, py, px2, py, TFT_BLACK);
    }

    for (int x = 0; x < XIANGQI_BOARD_WIDTH; x++) {
        int px = XIANGQI_OFFSET_X + x * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
        int py1, py2;

        if (x == 0 || x == XIANGQI_BOARD_WIDTH - 1) {
            py1 = XIANGQI_OFFSET_Y + XIANGQI_SQUARE_SIZE / 2;
            py2 = XIANGQI_OFFSET_Y + (XIANGQI_BOARD_HEIGHT - 1) * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
            tft_sprite_draw_line(&g_sprite, px, py1, px, py2, TFT_BLACK);
        } else {
            py1 = XIANGQI_OFFSET_Y + XIANGQI_SQUARE_SIZE / 2;
            py2 = XIANGQI_OFFSET_Y + 4 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
            tft_sprite_draw_line(&g_sprite, px, py1, px, py2, TFT_BLACK);
            py1 = XIANGQI_OFFSET_Y + 5 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
            py2 = XIANGQI_OFFSET_Y + 9 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
            tft_sprite_draw_line(&g_sprite, px, py1, px, py2, TFT_BLACK);
        }
    }

    tft_sprite_draw_line(&g_sprite,
        XIANGQI_OFFSET_X + 3 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_Y + 7 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_X + 5 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_Y + 9 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        TFT_BLACK);
    tft_sprite_draw_line(&g_sprite,
        XIANGQI_OFFSET_X + 5 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_Y + 7 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_X + 3 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_Y + 9 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        TFT_BLACK);

    tft_sprite_draw_line(&g_sprite,
        XIANGQI_OFFSET_X + 3 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_Y + 0 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_X + 5 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_Y + 2 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        TFT_BLACK);
    tft_sprite_draw_line(&g_sprite,
        XIANGQI_OFFSET_X + 5 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_Y + 0 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_X + 3 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_Y + 2 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        TFT_BLACK);
}

static void xiangqi_draw_piece(int x, int y, XiangqiPiece piece)
{
    if (piece.type == XIANGQI_PIECE_NONE) return;

    const uint8_t* bitmap = xiangqi_get_piece_bitmap(piece.type, xiangqi_get_piece_color(piece));
    if (!bitmap) return;

    int px = XIANGQI_OFFSET_X + x * XIANGQI_SQUARE_SIZE + (XIANGQI_SQUARE_SIZE - XIANGQI_PIECE_WIDTH) / 2;
    int py = XIANGQI_OFFSET_Y + y * XIANGQI_SQUARE_SIZE + (XIANGQI_SQUARE_SIZE - XIANGQI_PIECE_HEIGHT) / 2;

    uint16_t bg = RGB_TO_RGB565(0xCD, 0x85, 0x3F);
    for (int dy = 0; dy < XIANGQI_SQUARE_SIZE; dy++) {
        for (int dx = 0; dx < XIANGQI_SQUARE_SIZE; dx++) {
            int cx = dx - XIANGQI_SQUARE_SIZE / 2;
            int cy = dy - XIANGQI_SQUARE_SIZE / 2;
            if (cx * cx + cy * cy <= (XIANGQI_SQUARE_SIZE / 2 - 1) * (XIANGQI_SQUARE_SIZE / 2 - 1)) {
                tft_sprite_draw_pixel(&g_sprite, px + dx, py + dy, bg);
            }
        }
    }

    XiangqiColor color = xiangqi_get_piece_color(piece);
    uint16_t text_color = (color == XIANGQI_COLOR_RED) ? TFT_RED : TFT_BLACK;

    for (int dy = 0; dy < XIANGQI_PIECE_HEIGHT; dy++) {
        for (int dx = 0; dx < XIANGQI_PIECE_WIDTH; dx++) {
            int byte_idx = dy * 2 + (dx / 8);
            int bit_idx = 7 - (dx % 8);
            if (bitmap[byte_idx] & (1 << bit_idx)) {
                tft_sprite_draw_pixel(&g_sprite, px + dx, py + dy, text_color);
            }
        }
    }
}

static void xiangqi_draw_selection(int x, int y, uint16_t color)
{
    int px = XIANGQI_OFFSET_X + x * XIANGQI_SQUARE_SIZE;
    int py = XIANGQI_OFFSET_Y + y * XIANGQI_SQUARE_SIZE;
    int s = XIANGQI_SQUARE_SIZE;
    tft_sprite_draw_rect(&g_sprite, px + 1, py + 1, s - 2, s - 2, color);
    tft_sprite_draw_rect(&g_sprite, px + 2, py + 2, s - 4, s - 4, color);
}

static void xiangqi_draw_cursor(int x, int y)
{
    int px = XIANGQI_OFFSET_X + x * XIANGQI_SQUARE_SIZE;
    int py = XIANGQI_OFFSET_Y + y * XIANGQI_SQUARE_SIZE;
    int s = XIANGQI_SQUARE_SIZE;
    tft_sprite_fill_rect(&g_sprite, px, py, 4, 4, TFT_CYAN);
    tft_sprite_fill_rect(&g_sprite, px + s - 4, py, 4, 4, TFT_CYAN);
    tft_sprite_fill_rect(&g_sprite, px, py + s - 4, 4, 4, TFT_CYAN);
    tft_sprite_fill_rect(&g_sprite, px + s - 4, py + s - 4, 4, 4, TFT_CYAN);
}

static void xiangqi_draw_info(void)
{
    int info_x = XIANGQI_OFFSET_X + XIANGQI_BOARD_WIDTH * XIANGQI_SQUARE_SIZE + 2;
    if (info_x + 30 < TFT_WIDTH) {
        const char* turn_text = (g_ui->game.current_turn == XIANGQI_COLOR_RED) ? "RED" : "BLK";
        int y = 5;
        for (int i = 0; turn_text[i]; i++) {
            tft_sprite_draw_char(&g_sprite, info_x, y, turn_text[i],
                               (g_ui->game.current_turn == XIANGQI_COLOR_RED) ? TFT_RED : TFT_WHITE, 1);
            y += 8;
        }

        if (xiangqi_is_in_check(&g_ui->game, g_ui->game.current_turn)) {
            const char* check_text = "CHK";
            y = 40;
            for (int i = 0; check_text[i]; i++) {
                tft_sprite_draw_char(&g_sprite, info_x, y, check_text[i], TFT_YELLOW, 1);
                y += 8;
            }
        }
    }
}

static void render_game(void)
{
    xiangqi_draw_board_lines();

    for (int y = 0; y < XIANGQI_BOARD_HEIGHT; y++) {
        for (int x = 0; x < XIANGQI_BOARD_WIDTH; x++) {
            xiangqi_draw_piece(x, y, g_ui->game.board[y][x]);
        }
    }

    if (g_ui->has_selection) {
        xiangqi_draw_selection(g_ui->selected_x, g_ui->selected_y, TFT_GREEN);
    }

    xiangqi_draw_cursor(g_ui->cursor_x, g_ui->cursor_y);

    xiangqi_draw_info();

    if (g_ui->game.game_over || g_ui->game_over) {
        const char* result;
        if (g_ui->game.winner == XIANGQI_COLOR_RED) {
            result = "RED WINS!";
        } else if (g_ui->game.winner == XIANGQI_COLOR_BLACK) {
            result = "BLK WINS!";
        } else {
            result = "DRAW!";
        }
        int result_x = (TFT_WIDTH - tft_sprite_text_width(result, 1)) / 2;
        int result_y = (TFT_HEIGHT - 8) / 2;
        tft_sprite_fill_rect(&g_sprite, result_x - 4, result_y - 4, tft_sprite_text_width(result, 1) + 8, 16, COLOR_DIALOG);
        int x = result_x;
        for (const char* p = result; *p; p++) {
            tft_sprite_draw_char(&g_sprite, x, result_y, *p, TFT_YELLOW, 1);
            x += 6;
        }

        const char* hint = "B: Back";
        int hint_x = (TFT_WIDTH - tft_sprite_text_width(hint, 1)) / 2;
        x = hint_x;
        for (const char* p = hint; *p; p++) {
            tft_sprite_draw_char(&g_sprite, x, result_y + 16, *p, TFT_LIGHTGREY, 1);
            x += 6;
        }
    }

    if (g_ui->show_message) {
        int msg_x = (TFT_WIDTH - tft_sprite_text_width(g_ui->message_text, 1)) / 2;
        int msg_y = 50;
        tft_sprite_fill_rect(&g_sprite, msg_x - 4, msg_y - 4, tft_sprite_text_width(g_ui->message_text, 1) + 8, 16, COLOR_DIALOG);
        int x = msg_x;
        for (const char* p = g_ui->message_text; *p; p++) {
            tft_sprite_draw_char(&g_sprite, x, msg_y, *p, TFT_WHITE, 1);
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
        case XQ_SCREEN_SPLASH:
            render_splash();
            break;
        case XQ_SCREEN_MAIN_MENU:
            render_main_menu();
            break;
        case XQ_SCREEN_BLUETOOTH_MENU:
            render_bluetooth_menu();
            break;
        case XQ_SCREEN_BLUETOOTH_SCAN:
            render_bluetooth_scan();
            break;
        case XQ_SCREEN_BLUETOOTH_WAITING:
            render_bluetooth_waiting();
            break;
        case XQ_SCREEN_GAME:
            render_game();
            break;
        case XQ_SCREEN_EXIT_CONFIRM:
            render_exit_confirm();
            break;
    }

    tft_sprite_push(&g_sprite, 0, 0);
}

void xiangqi_ui_render_locked(void)
{
    if (!g_ui || !g_sprite_created) return;

    int64_t now = esp_timer_get_time();
    if (now - g_last_render < 33000) return;
    g_last_render = now;

    render_internal();
}

void xiangqi_ui_render(void)
{
    if (!g_ui || !g_sprite_created) return;

    ui_lock_acquire();
    xiangqi_ui_render_locked();
    ui_lock_release();
}

static void xiangqi_show_message(const char* text)
{
    strncpy(g_ui->message_text, text, sizeof(g_ui->message_text) - 1);
    g_ui->message_text[sizeof(g_ui->message_text) - 1] = '\0';
    g_ui->show_message = true;
    g_ui->message_timer = 20;
}

void xiangqi_ui_handle_key(int key)
{
    if (!g_ui) return;

    ui_lock_acquire();

    switch (g_ui->current_screen) {
        case XQ_SCREEN_SPLASH:
            sound_play(SOUND_KEY_CLICK);
            xiangqi_ui_switch_screen(XQ_SCREEN_MAIN_MENU);
            break;

        case XQ_SCREEN_MAIN_MENU:
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
                    xiangqi_ui_switch_screen(XQ_SCREEN_GAME);
                } else if (g_ui->menu_index == 1) {
                    xiangqi_ui_switch_screen(XQ_SCREEN_BLUETOOTH_MENU);
                } else if (g_ui->menu_index == 2) {
                    s_should_exit = true;
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                s_should_exit = true;
            }
            break;

        case XQ_SCREEN_BLUETOOTH_MENU:
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
                        xiangqi_ui_switch_screen(XQ_SCREEN_BLUETOOTH_WAITING);
                    }
                } else if (g_ui->menu_index == 1) {
                    if (bt_chess_start_scanning()) {
                        strcpy(g_ui->bluetooth_status, "Scanning...");
                        xiangqi_ui_switch_screen(XQ_SCREEN_BLUETOOTH_SCAN);
                    }
                } else {
                    xiangqi_ui_switch_screen(XQ_SCREEN_MAIN_MENU);
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                xiangqi_ui_switch_screen(XQ_SCREEN_MAIN_MENU);
            }
            break;

        case XQ_SCREEN_BLUETOOTH_SCAN:
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
                        xiangqi_ui_switch_screen(XQ_SCREEN_BLUETOOTH_WAITING);
                    }
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                bt_chess_stop_scanning();
                xiangqi_ui_switch_screen(XQ_SCREEN_BLUETOOTH_MENU);
            }
            break;

        case XQ_SCREEN_BLUETOOTH_WAITING:
            if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                bt_chess_disconnect();
                bt_chess_stop_hosting();
                bt_chess_stop_scanning();
                xiangqi_ui_switch_screen(XQ_SCREEN_BLUETOOTH_MENU);
            }
            break;

        case XQ_SCREEN_EXIT_CONFIRM:
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
                    xiangqi_ui_switch_screen(XQ_SCREEN_MAIN_MENU);
                } else {
                    xiangqi_ui_switch_screen(g_ui->previous_screen);
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                xiangqi_ui_switch_screen(g_ui->previous_screen);
            }
            break;

        case XQ_SCREEN_GAME:
            if (g_ui->game.game_over || g_ui->game_over) {
                if (key == KEY_B || key == KEY_A) {
                    sound_play(SOUND_KEY_CLICK);
                    xiangqi_ui_switch_screen(XQ_SCREEN_MAIN_MENU);
                }
                break;
            }

            if (key == KEY_UP) {
                if (g_ui->cursor_y > 0) {
                    g_ui->cursor_y--;
                    sound_play(SOUND_KEY_CLICK);
                }
            } else if (key == KEY_DOWN) {
                if (g_ui->cursor_y < XIANGQI_BOARD_HEIGHT - 1) {
                    g_ui->cursor_y++;
                    sound_play(SOUND_KEY_CLICK);
                }
            } else if (key == KEY_LEFT) {
                if (g_ui->cursor_x > 0) {
                    g_ui->cursor_x--;
                    sound_play(SOUND_KEY_CLICK);
                }
            } else if (key == KEY_RIGHT) {
                if (g_ui->cursor_x < XIANGQI_BOARD_WIDTH - 1) {
                    g_ui->cursor_x++;
                    sound_play(SOUND_KEY_CLICK);
                }
            } else if (key == KEY_A) {
                XiangqiPiece piece = g_ui->game.board[g_ui->cursor_y][g_ui->cursor_x];

                if (g_ui->has_selection) {
                    if (g_ui->selected_x == g_ui->cursor_x && g_ui->selected_y == g_ui->cursor_y) {
                        sound_play(SOUND_KEY_CLICK);
                        g_ui->has_selection = false;
                        g_ui->selected_x = -1;
                        g_ui->selected_y = -1;
                    } else if (xiangqi_is_valid_move(&g_ui->game, g_ui->selected_x, g_ui->selected_y,
                                                g_ui->cursor_x, g_ui->cursor_y)) {
                        XiangqiPiece target = g_ui->game.board[g_ui->cursor_y][g_ui->cursor_x];
                        bool is_capture = (target.type != XIANGQI_PIECE_NONE);

                        if (xiangqi_make_move(&g_ui->game, g_ui->selected_x, g_ui->selected_y,
                                          g_ui->cursor_x, g_ui->cursor_y)) {
                            if (is_capture) {
                                sound_play(SOUND_CAPTURE);
                            } else {
                                sound_play(SOUND_MOVE);
                            }

                            if (xiangqi_is_in_check(&g_ui->game, g_ui->game.current_turn)) {
                                if (xiangqi_is_checkmate(&g_ui->game, g_ui->game.current_turn)) {
                                    sound_play(SOUND_CHECKMATE);
                                    g_ui->game.game_over = true;
                                    g_ui->game.winner = (g_ui->game.current_turn == XIANGQI_COLOR_RED)
                                                      ? XIANGQI_COLOR_BLACK : XIANGQI_COLOR_RED;
                                } else {
                                    sound_play(SOUND_CHECK);
                                }
                            }
                        } else {
                            sound_play(SOUND_INVALID_MOVE);
                            xiangqi_show_message("Invalid!");
                        }
                        g_ui->has_selection = false;
                        g_ui->selected_x = -1;
                        g_ui->selected_y = -1;
                    } else if (piece.type != XIANGQI_PIECE_NONE &&
                           xiangqi_get_piece_color(piece) == g_ui->game.current_turn) {
                        sound_play(SOUND_MENU_SELECT);
                        g_ui->selected_x = g_ui->cursor_x;
                        g_ui->selected_y = g_ui->cursor_y;
                    } else {
                        sound_play(SOUND_INVALID_MOVE);
                        g_ui->has_selection = false;
                        g_ui->selected_x = -1;
                        g_ui->selected_y = -1;
                    }
                } else {
                    if (piece.type != XIANGQI_PIECE_NONE &&
                        xiangqi_get_piece_color(piece) == g_ui->game.current_turn) {
                        sound_play(SOUND_MENU_SELECT);
                        g_ui->selected_x = g_ui->cursor_x;
                        g_ui->selected_y = g_ui->cursor_y;
                        g_ui->has_selection = true;
                    } else if (piece.type != XIANGQI_PIECE_NONE) {
                        sound_play(SOUND_INVALID_MOVE);
                    } else {
                        sound_play(SOUND_INVALID_MOVE);
                    }
                }
            } else if (key == KEY_B) {
                if (g_ui->has_selection) {
                    g_ui->has_selection = false;
                    g_ui->selected_x = -1;
                    g_ui->selected_y = -1;
                    sound_play(SOUND_KEY_CLICK);
                } else {
                    sound_play(SOUND_KEY_CLICK);
                    xiangqi_ui_switch_screen(XQ_SCREEN_EXIT_CONFIRM);
                }
            }
            break;
    }

    ui_lock_release();
}

void xiangqi_ui_update(void)
{
    if (!g_ui) return;

    if (g_ui->current_screen == XQ_SCREEN_SPLASH) {
        int64_t now = esp_timer_get_time();
        if (now - g_ui->splash_start_time > 3000000) {
            xiangqi_ui_switch_screen(XQ_SCREEN_MAIN_MENU);
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
