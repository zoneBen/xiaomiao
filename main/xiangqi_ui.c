#include "xiangqi_ui.h"
#include "xiangqi_pieces.h"
#include "sound.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "XiangqiUI";

// ==================== UI状态 ====================

typedef struct {
    XiangqiGame game;
    int cursor_x;
    int cursor_y;
    int selected_x;
    int selected_y;
    bool has_selection;
    bool game_over;
    bool show_message;
    char message_text[32];
    int message_timer;
} XiangqiUIState;

static XiangqiUIState* g_ui = NULL;
static TFT_Sprite g_sprite;
static bool g_sprite_created = false;
static int64_t g_last_render = 0;
static bool s_should_exit = false;

// 棋盘偏移，让棋盘居中
#define XIANGQI_OFFSET_X 4
#define XIANGQI_OFFSET_Y 0
#define XIANGQI_SQUARE_SIZE 13  // 13像素一格，9x10 = 117x130

// ==================== 初始化 ====================

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
    g_ui->cursor_x = 4;
    g_ui->cursor_y = 9;
    g_ui->selected_x = -1;
    g_ui->selected_y = -1;
    g_ui->has_selection = false;
    g_ui->game_over = false;

    if (tft_sprite_create(&g_sprite, TFT_WIDTH, TFT_HEIGHT) == ESP_OK) {
        g_sprite_created = true;
    }
}

void xiangqi_ui_deinit(void)
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

void xiangqi_ui_switch_to_game(void)
{
    s_should_exit = false;
    if (!g_ui) {
        xiangqi_ui_init();
    } else {
        xiangqi_game_reset(&g_ui->game);
        g_ui->cursor_x = 4;
        g_ui->cursor_y = 9;
        g_ui->selected_x = -1;
        g_ui->selected_y = -1;
        g_ui->has_selection = false;
        g_ui->game_over = false;
    }
}

bool xiangqi_ui_should_exit(void)
{
    bool result = s_should_exit;
    s_should_exit = false;
    return result;
}

// ==================== 渲染棋盘 ====================

static void xiangqi_draw_board_lines(void)
{
    // 背景色
    tft_sprite_fill_screen(&g_sprite, COLOR_BACKGROUND);

    // 棋盘区域背景 - 偏黄/棕色的木质感觉
    uint16_t board_bg = RGB_TO_RGB565(0x8B, 0x69, 0x14);
    tft_sprite_fill_rect(&g_sprite, XIANGQI_OFFSET_X, XIANGQI_OFFSET_Y,
                        XIANGQI_BOARD_WIDTH * XIANGQI_SQUARE_SIZE,
                        XIANGQI_BOARD_HEIGHT * XIANGQI_SQUARE_SIZE,
                        board_bg);

    // 画横线
    for (int y = 0; y < XIANGQI_BOARD_HEIGHT; y++) {
        int py = XIANGQI_OFFSET_Y + y * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
        int px1 = XIANGQI_OFFSET_X + XIANGQI_SQUARE_SIZE / 2;
        int px2 = XIANGQI_OFFSET_X + (XIANGQI_BOARD_WIDTH - 1) * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
        tft_sprite_draw_line(&g_sprite, px1, py, px2, py, TFT_BLACK);
    }

    // 画竖线
    for (int x = 0; x < XIANGQI_BOARD_WIDTH; x++) {
        int px = XIANGQI_OFFSET_X + x * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
        int py1, py2;

        if (x == 0 || x == XIANGQI_BOARD_WIDTH - 1) {
            // 最左和最右竖线是完整的
            py1 = XIANGQI_OFFSET_Y + XIANGQI_SQUARE_SIZE / 2;
            py2 = XIANGQI_OFFSET_Y + (XIANGQI_BOARD_HEIGHT - 1) * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
        } else {
            // 其他竖线在楚河汉界处断开
            py1 = XIANGQI_OFFSET_Y + XIANGQI_SQUARE_SIZE / 2;
            py2 = XIANGQI_OFFSET_Y + 4 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
            tft_sprite_draw_line(&g_sprite, px, py1, px, py2, TFT_BLACK);

            py1 = XIANGQI_OFFSET_Y + 5 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
            py2 = XIANGQI_OFFSET_Y + 9 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
            tft_sprite_draw_line(&g_sprite, px, py1, px, py2, TFT_BLACK);
            continue;
        }
        tft_sprite_draw_line(&g_sprite, px, py1, px, py2, TFT_BLACK);
    }

    // 画九宫格斜线
    // 红方九宫（下方）
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

    // 黑方九宫（上方）
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

// ==================== 渲染棋子 ====================

static void xiangqi_draw_piece(int x, int y, XiangqiPiece piece)
{
    if (piece.type == XIANGQI_PIECE_NONE) return;

    const uint8_t* bitmap = xiangqi_get_piece_bitmap(piece.type, xiangqi_get_piece_color(piece));
    if (!bitmap) return;

    int px = XIANGQI_OFFSET_X + x * XIANGQI_SQUARE_SIZE + (XIANGQI_SQUARE_SIZE - XIANGQI_PIECE_WIDTH) / 2;
    int py = XIANGQI_OFFSET_Y + y * XIANGQI_SQUARE_SIZE + (XIANGQI_SQUARE_SIZE - XIANGQI_PIECE_HEIGHT) / 2;

    uint16_t bg = RGB_TO_RGB565(0xCD, 0x85, 0x3F); // 棋子背景色
    // 画圆形背景
    for (int dy = 0; dy < XIANGQI_SQUARE_SIZE; dy++) {
        for (int dx = 0; dx < XIANGQI_SQUARE_SIZE; dx++) {
            int cx = dx - XIANGQI_SQUARE_SIZE / 2;
            int cy = dy - XIANGQI_SQUARE_SIZE / 2;
            if (cx * cx + cy * cy <= (XIANGQI_SQUARE_SIZE / 2 - 1) * (XIANGQI_SQUARE_SIZE / 2 - 1)) {
                tft_sprite_draw_pixel(&g_sprite, px + dx, py + dy, bg);
            }
        }
    }

    // 画文字（位图）
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

// ==================== 渲染选择框和光标 ====================

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

    // 画四个角
    tft_sprite_fill_rect(&g_sprite, px, py, 4, 4, TFT_CYAN);
    tft_sprite_fill_rect(&g_sprite, px + s - 4, py, 4, 4, TFT_CYAN);
    tft_sprite_fill_rect(&g_sprite, px, py + s - 4, 4, 4, TFT_CYAN);
    tft_sprite_fill_rect(&g_sprite, px + s - 4, py + s - 4, 4, 4, TFT_CYAN);
}

// ==================== 渲染信息 ====================

static void xiangqi_draw_info(void)
{
    // 显示当前回合
    int info_x = XIANGQI_OFFSET_X + XIANGQI_BOARD_WIDTH * XIANGQI_SQUARE_SIZE + 2;
    if (info_x + 30 < TFT_WIDTH) {
        const char* turn_text = (g_ui->game.current_turn == XIANGQI_COLOR_RED) ? "RED" : "BLK";
        int y = 5;
        for (int i = 0; turn_text[i]; i++) {
            tft_sprite_draw_char(&g_sprite, info_x, y, turn_text[i],
                               (g_ui->game.current_turn == XIANGQI_COLOR_RED) ? TFT_RED : TFT_WHITE, 1);
            y += 8;
        }

        // 将军提示
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

// ==================== 主渲染函数 ====================

static void xiangqi_render_internal(void)
{
    xiangqi_draw_board_lines();

    // 画棋子
    for (int y = 0; y < XIANGQI_BOARD_HEIGHT; y++) {
        for (int x = 0; x < XIANGQI_BOARD_WIDTH; x++) {
            xiangqi_draw_piece(x, y, g_ui->game.board[y][x]);
        }
    }

    // 画选择框
    if (g_ui->has_selection) {
        xiangqi_draw_selection(g_ui->selected_x, g_ui->selected_y, TFT_GREEN);
    }

    // 画光标
    xiangqi_draw_cursor(g_ui->cursor_x, g_ui->cursor_y);

    // 画信息
    xiangqi_draw_info();

    // 游戏结束提示
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

        const char* hint = "B:Back";
        int hint_x = (TFT_WIDTH - tft_sprite_text_width(hint, 1)) / 2;
        x = hint_x;
        for (const char* p = hint; *p; p++) {
            tft_sprite_draw_char(&g_sprite, x, result_y + 16, *p, TFT_LIGHTGREY, 1);
            x += 6;
        }
    }

    // 消息提示
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

void xiangqi_ui_render(void)
{
    if (!g_ui || !g_sprite_created) return;

    int64_t now = esp_timer_get_time();
    if (now - g_last_render < 33000) return;
    g_last_render = now;

    ui_lock_acquire();
    xiangqi_render_internal();
    tft_sprite_push(&g_sprite, 0, 0);
    ui_lock_release();
}

// ==================== 按键处理 ====================

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

    if (g_ui->game.game_over || g_ui->game_over) {
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
            // 尝试移动
            if (xiangqi_is_valid_move(&g_ui->game, g_ui->selected_x, g_ui->selected_y,
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

                    // 检查将军/将死
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
                // 选择另一个自己的棋子
                sound_play(SOUND_MENU_SELECT);
                g_ui->selected_x = g_ui->cursor_x;
                g_ui->selected_y = g_ui->cursor_y;
            } else {
                // 无效移动，取消选择
                sound_play(SOUND_INVALID_MOVE);
                g_ui->has_selection = false;
                g_ui->selected_x = -1;
                g_ui->selected_y = -1;
            }
        } else {
            // 选择棋子
            if (piece.type != XIANGQI_PIECE_NONE &&
                xiangqi_get_piece_color(piece) == g_ui->game.current_turn) {
                sound_play(SOUND_MENU_SELECT);
                g_ui->selected_x = g_ui->cursor_x;
                g_ui->selected_y = g_ui->cursor_y;
                g_ui->has_selection = true;
            } else if (piece.type != XIANGQI_PIECE_NONE) {
                // 选了对方的棋子
                sound_play(SOUND_INVALID_MOVE);
            } else {
                // 选了空格
                sound_play(SOUND_INVALID_MOVE);
            }
        }
    } else if (key == KEY_B) {
        if (g_ui->has_selection) {
            // 取消选择
            g_ui->has_selection = false;
            g_ui->selected_x = -1;
            g_ui->selected_y = -1;
            sound_play(SOUND_KEY_CLICK);
        } else {
            // 返回菜单
            s_should_exit = true;
            sound_play(SOUND_KEY_CLICK);
        }
    }

    ui_lock_release();
}

void xiangqi_ui_update(void)
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
