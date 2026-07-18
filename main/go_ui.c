#include "go_ui.h"
#include "sound.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "GoUI";

// UI状态
typedef struct {
    GoGame game;
    int cursor_x;
    int cursor_y;
    bool game_over;
    bool show_message;
    char message_text[32];
    int message_timer;
} GoUIState;

static GoUIState* g_ui = NULL;
static TFT_Sprite g_sprite;
static bool g_sprite_created = false;
static int64_t g_last_render = 0;
static bool s_should_exit = false;

// 棋盘格子大小和偏移 - 9x9棋盘
#define GO_SQUARE_SIZE 12
#define GO_OFFSET_X 26
#define GO_OFFSET_Y 8

void go_ui_init(void)
{
    if (g_ui != NULL) {
        return;
    }

    ESP_LOGI(TAG, "Initializing Go UI");

    g_ui = (GoUIState*)calloc(1, sizeof(GoUIState));
    if (!g_ui) {
        ESP_LOGE(TAG, "Failed to allocate Go UI");
        return;
    }

    go_game_init(&g_ui->game);
    g_ui->cursor_x = GO_BOARD_SIZE / 2;
    g_ui->cursor_y = GO_BOARD_SIZE / 2;
    g_ui->game_over = false;

    if (tft_sprite_create(&g_sprite, TFT_WIDTH, TFT_HEIGHT) == ESP_OK) {
        g_sprite_created = true;
    }
}

void go_ui_deinit(void)
{
    if (g_ui != NULL) {
        if (g_sprite_created) {
            tft_sprite_delete(&g_sprite);
        }
        free(g_ui);
        g_ui = NULL;
        g_sprite_created = false;
    }
}

void go_ui_switch_to_game(void)
{
    s_should_exit = false;
    if (!g_ui) {
        go_ui_init();
    } else {
        go_game_reset(&g_ui->game);
        g_ui->cursor_x = GO_BOARD_SIZE / 2;
        g_ui->cursor_y = GO_BOARD_SIZE / 2;
        g_ui->game_over = false;
    }
}

bool go_ui_should_exit(void)
{
    bool result = s_should_exit;
    s_should_exit = false;
    return result;
}

// 画棋盘
static void go_draw_board(void)
{
    tft_sprite_fill_screen(&g_sprite, COLOR_BACKGROUND);

    // 棋盘底色
    uint16_t board_color = RGB_TO_RGB565(0xD2, 0x69, 0x1E);
    int board_w = (GO_BOARD_SIZE - 1) * GO_SQUARE_SIZE;
    int board_h = (GO_BOARD_SIZE - 1) * GO_SQUARE_SIZE;
    tft_sprite_fill_rect(&g_sprite, GO_OFFSET_X, GO_OFFSET_Y, board_w, board_h, board_color);

    // 画网格线
    for (int i = 0; i < GO_BOARD_SIZE; i++) {
        int x = GO_OFFSET_X + i * GO_SQUARE_SIZE;
        int y = GO_OFFSET_Y + i * GO_SQUARE_SIZE;

        // 竖线
        tft_sprite_draw_line(&g_sprite, x, GO_OFFSET_Y,
                             x, GO_OFFSET_Y + board_h - 1, TFT_BLACK);
        // 横线
        tft_sprite_draw_line(&g_sprite, GO_OFFSET_X, y,
                             GO_OFFSET_X + board_w - 1, y, TFT_BLACK);
    }

    // 画星位 - 9x9棋盘有5个星位
    int star_positions[5][2] = {{2, 2}, {6, 2}, {4, 4}, {2, 6}, {6, 6}};
    for (int i = 0; i < 5; i++) {
        int x = GO_OFFSET_X + star_positions[i][0] * GO_SQUARE_SIZE;
        int y = GO_OFFSET_Y + star_positions[i][1] * GO_SQUARE_SIZE;
        tft_sprite_fill_rect(&g_sprite, x - 2, y - 2, 5, 5, TFT_BLACK);
    }
}

// 画棋子
static void go_draw_pieces(void)
{
    for (int y = 0; y < GO_BOARD_SIZE; y++) {
        for (int x = 0; x < GO_BOARD_SIZE; x++) {
            GoPiece piece = g_ui->game.board[y][x];
            if (piece == GO_EMPTY) continue;

            int px = GO_OFFSET_X + x * GO_SQUARE_SIZE;
            int py = GO_OFFSET_Y + y * GO_SQUARE_SIZE;

            // 画圆形棋子
            int r = GO_SQUARE_SIZE / 2 - 1;
            for (int dy = -r; dy <= r; dy++) {
                for (int dx = -r; dx <= r; dx++) {
                    if (dx * dx + dy * dy <= r * r) {
                        uint16_t color = (piece == GO_BLACK) ? TFT_BLACK : TFT_WHITE;
                        tft_sprite_draw_pixel(&g_sprite, px + dx, py + dy, color);
                    }
                }
            }
        }
    }
}

// 画光标
static void go_draw_cursor(void)
{
    int px = GO_OFFSET_X + g_ui->cursor_x * GO_SQUARE_SIZE;
    int py = GO_OFFSET_Y + g_ui->cursor_y * GO_SQUARE_SIZE;

    // 只有在空位置才显示光标
    if (g_ui->game.board[g_ui->cursor_y][g_ui->cursor_x] != GO_EMPTY) {
        return;
    }

    uint16_t cursor_color = (g_ui->game.current_player == GO_BLACK) ? TFT_WHITE : TFT_BLACK;

    // 画四角的标记
    int s = GO_SQUARE_SIZE / 2 - 2;
    tft_sprite_fill_rect(&g_sprite, px - s, py - s, 3, 3, cursor_color);
    tft_sprite_fill_rect(&g_sprite, px + s - 2, py - s, 3, 3, cursor_color);
    tft_sprite_fill_rect(&g_sprite, px - s, py + s - 2, 3, 3, cursor_color);
    tft_sprite_fill_rect(&g_sprite, px + s - 2, py + s - 2, 3, 3, cursor_color);
}

// 画信息栏
static void go_draw_info(void)
{
    // 当前玩家
    const char* turn_text = (g_ui->game.current_player == GO_BLACK) ?
                           "Turn: Black" : "Turn: White";
    int x = 5;
    int y = 120;
    for (const char* p = turn_text; *p; p++) {
        tft_sprite_draw_char(&g_sprite, x, y, *p, TFT_LIGHTGREY, 1);
        x += 6;
    }

    // 提示
    const char* hint = "A:Place B:Back";
    x = 105;
    for (const char* p = hint; *p; p++) {
        tft_sprite_draw_char(&g_sprite, x, y, *p, TFT_LIGHTGREY, 1);
        x += 6;
    }
}

static void go_render_internal(void)
{
    go_draw_board();
    go_draw_pieces();
    go_draw_cursor();
    go_draw_info();

    // 游戏结束提示
    if (g_ui->game.game_over) {
        const char* result = "Game Over!";
        int x = (TFT_WIDTH - tft_sprite_text_width(result, 1)) / 2;
        int y = 50;
        tft_sprite_fill_rect(&g_sprite, x - 5, y - 5, tft_sprite_text_width(result, 1) + 10, 18, COLOR_DIALOG);
        for (const char* p = result; *p; p++) {
            tft_sprite_draw_char(&g_sprite, x, y, *p, TFT_YELLOW, 1);
            x += 6;
        }

        const char* hint = "A/B:Back";
        x = (TFT_WIDTH - tft_sprite_text_width(hint, 1)) / 2;
        y = 70;
        for (const char* p = hint; *p; p++) {
            tft_sprite_draw_char(&g_sprite, x, y, *p, TFT_LIGHTGREY, 1);
            x += 6;
        }
    }

    // 消息提示
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

void go_ui_render(void)
{
    if (!g_ui || !g_sprite_created) return;

    int64_t now = esp_timer_get_time();
    if (now - g_last_render < 33000) return;
    g_last_render = now;

    ui_lock_acquire();
    go_render_internal();
    tft_sprite_push(&g_sprite, 0, 0);
    ui_lock_release();
}

void go_ui_handle_key(int key)
{
    if (!g_ui) return;

    ui_lock_acquire();

    if (g_ui->game.game_over) {
        if (key == KEY_B || key == KEY_A) {
            sound_play(SOUND_KEY_CLICK);
            s_should_exit = true;
        }
        ui_lock_release();
        return;
    }

    if (key == KEY_UP) {
        if (g_ui->cursor_y > 0) {
            g_ui->cursor_y--;
            sound_play(SOUND_KEY_CLICK);
        }
    } else if (key == KEY_DOWN) {
        if (g_ui->cursor_y < GO_BOARD_SIZE - 1) {
            g_ui->cursor_y++;
            sound_play(SOUND_KEY_CLICK);
        }
    } else if (key == KEY_LEFT) {
        if (g_ui->cursor_x > 0) {
            g_ui->cursor_x--;
            sound_play(SOUND_KEY_CLICK);
        }
    } else if (key == KEY_RIGHT) {
        if (g_ui->cursor_x < GO_BOARD_SIZE - 1) {
            g_ui->cursor_x++;
            sound_play(SOUND_KEY_CLICK);
        }
    } else if (key == KEY_A) {
        if (go_place_piece(&g_ui->game, g_ui->cursor_x, g_ui->cursor_y)) {
            sound_play(SOUND_MOVE);
        } else {
            sound_play(SOUND_INVALID_MOVE);
        }
    } else if (key == KEY_B) {
        sound_play(SOUND_KEY_CLICK);
        s_should_exit = true;
    }

    ui_lock_release();
}

void go_ui_update(void)
{
    if (!g_ui) return;

    if (g_ui->show_message) {
        if (g_ui->message_timer > 0) {
            g_ui->message_timer--;
        } else {
            g_ui->show_message = false;
        }
    }
}