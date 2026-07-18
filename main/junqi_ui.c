#include "junqi_ui.h"
#include "sound.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "JunQiUI";

// UI状态
typedef struct {
    JunqiGame game;
    int cursor_x;
    int cursor_y;
    bool game_over;
    bool show_message;
    char message_text[32];
    int message_timer;
} JunqiUIState;

static JunqiUIState* s_ui = NULL;
static TFT_Sprite s_sprite;
static bool s_sprite_created = false;
static int64_t s_last_render = 0;
static bool s_should_exit = false;

// 棋盘格子大小和偏移
#define JUNQI_CELL_SIZE 18
#define JUNQI_OFFSET_X 10
#define JUNQI_OFFSET_Y 5

// 获取棋子名称
static const char* junqi_get_piece_name(JunqiPiece piece)
{
    switch (piece) {
        case JUNQI_PIECE_RED_SILING: return "司";
        case JUNQI_PIECE_RED_JUNZHANG: return "军";
        case JUNQI_PIECE_RED_SHIZHANG: return "师";
        case JUNQI_PIECE_RED_LVZHANG: return "旅";
        case JUNQI_PIECE_RED_TUANZHANG: return "团";
        case JUNQI_PIECE_RED_YINGZHANG: return "营";
        case JUNQI_PIECE_RED_LIANZHANG: return "连";
        case JUNQI_PIECE_RED_PAIZHANG: return "排";
        case JUNQI_PIECE_RED_GONGBING: return "工";
        case JUNQI_PIECE_RED_DILEI: return "雷";
        case JUNQI_PIECE_RED_ZHADAN: return "炸";
        case JUNQI_PIECE_RED_JUNQI: return "旗";
        case JUNQI_PIECE_BLUE_SILING: return "司";
        case JUNQI_PIECE_BLUE_JUNZHANG: return "军";
        case JUNQI_PIECE_BLUE_SHIZHANG: return "师";
        case JUNQI_PIECE_BLUE_LVZHANG: return "旅";
        case JUNQI_PIECE_BLUE_TUANZHANG: return "团";
        case JUNQI_PIECE_BLUE_YINGZHANG: return "营";
        case JUNQI_PIECE_BLUE_LIANZHANG: return "连";
        case JUNQI_PIECE_BLUE_PAIZHANG: return "排";
        case JUNQI_PIECE_BLUE_GONGBING: return "工";
        case JUNQI_PIECE_BLUE_DILEI: return "雷";
        case JUNQI_PIECE_BLUE_ZHADAN: return "炸";
        case JUNQI_PIECE_BLUE_JUNQI: return "旗";
        default: return "";
    }
}

void junqi_ui_init(void)
{
    if (s_ui != NULL) {
        return;
    }

    ESP_LOGI(TAG, "Initializing JunQi UI");

    s_ui = (JunqiUIState*)calloc(1, sizeof(JunqiUIState));
    if (!s_ui) {
        ESP_LOGE(TAG, "Failed to allocate JunQi UI");
        return;
    }

    junqi_game_init(&s_ui->game);
    s_ui->cursor_x = 3;
    s_ui->cursor_y = 4;
    s_ui->game_over = false;

    if (tft_sprite_create(&s_sprite, TFT_WIDTH, TFT_HEIGHT) == ESP_OK) {
        s_sprite_created = true;
    }
}

void junqi_ui_deinit(void)
{
    if (s_ui != NULL) {
        if (s_sprite_created) {
            tft_sprite_delete(&s_sprite);
        }
        free(s_ui);
        s_ui = NULL;
        s_sprite_created = false;
    }
}

void junqi_ui_switch_to_game(void)
{
    s_should_exit = false;
    if (!s_ui) {
        junqi_ui_init();
    } else {
        junqi_game_reset(&s_ui->game);
        s_ui->cursor_x = 3;
        s_ui->cursor_y = 4;
        s_ui->game_over = false;
    }
}

bool junqi_ui_should_exit(void)
{
    bool result = s_should_exit;
    s_should_exit = false;
    return result;
}

// 画棋盘
static void junqi_draw_board(void)
{
    tft_sprite_fill_screen(&s_sprite, COLOR_BACKGROUND);

    // 棋盘底色
    uint16_t board_color = RGB_TO_RGB565(0xD2, 0x69, 0x1E);
    tft_sprite_fill_rect(&s_sprite, JUNQI_OFFSET_X, JUNQI_OFFSET_Y,
                         JUNQI_BOARD_WIDTH * JUNQI_CELL_SIZE - 1,
                         JUNQI_BOARD_HEIGHT * JUNQI_CELL_SIZE - 1,
                         board_color);

    // 画网格
    for (int y = 0; y <= JUNQI_BOARD_HEIGHT; y++) {
        tft_sprite_draw_line(&s_sprite,
                            JUNQI_OFFSET_X,
                            JUNQI_OFFSET_Y + y * JUNQI_CELL_SIZE,
                            JUNQI_OFFSET_X + (JUNQI_BOARD_WIDTH - 1) * JUNQI_CELL_SIZE,
                            JUNQI_OFFSET_Y + y * JUNQI_CELL_SIZE,
                            TFT_BLACK);
    }
    for (int x = 0; x <= JUNQI_BOARD_WIDTH; x++) {
        tft_sprite_draw_line(&s_sprite,
                            JUNQI_OFFSET_X + x * JUNQI_CELL_SIZE,
                            JUNQI_OFFSET_Y,
                            JUNQI_OFFSET_X + x * JUNQI_CELL_SIZE,
                            JUNQI_OFFSET_Y + (JUNQI_BOARD_HEIGHT - 1) * JUNQI_CELL_SIZE,
                            TFT_BLACK);
    }

    // 画营（X标记）
    int camps[6][2] = {
        {1, 2}, {3, 1}, {5, 2},
        {1, 6}, {3, 7}, {5, 6}
    };
    for (int i = 0; i < 6; i++) {
        int cx = JUNQI_OFFSET_X + camps[i][0] * JUNQI_CELL_SIZE + JUNQI_CELL_SIZE / 2;
        int cy = JUNQI_OFFSET_Y + camps[i][1] * JUNQI_CELL_SIZE + JUNQI_CELL_SIZE / 2;
        int s = 4;
        tft_sprite_draw_line(&s_sprite, cx - s, cy - s, cx + s, cy + s, TFT_BLUE);
        tft_sprite_draw_line(&s_sprite, cx + s, cy - s, cx - s, cy + s, TFT_BLUE);
    }

    // 画大本营（O标记）
    int hqs[4][2] = {{2, 0}, {4, 0}, {2, 8}, {4, 8}};
    for (int i = 0; i < 4; i++) {
        int cx = JUNQI_OFFSET_X + hqs[i][0] * JUNQI_CELL_SIZE + JUNQI_CELL_SIZE / 2;
        int cy = JUNQI_OFFSET_Y + hqs[i][1] * JUNQI_CELL_SIZE + JUNQI_CELL_SIZE / 2;
        tft_sprite_draw_circle(&s_sprite, cx, cy, 5, TFT_BLUE);
    }
}

// 画棋子
static void junqi_draw_pieces(void)
{
    for (int y = 0; y < JUNQI_BOARD_HEIGHT; y++) {
        for (int x = 0; x < JUNQI_BOARD_WIDTH; x++) {
            JunqiPiece piece = s_ui->game.board[y][x];
            if (piece == JUNQI_PIECE_NONE) continue;

            int px = JUNQI_OFFSET_X + x * JUNQI_CELL_SIZE;
            int py = JUNQI_OFFSET_Y + y * JUNQI_CELL_SIZE;

            // 棋子底色
            JunqiPlayer player = junqi_get_piece_player(piece);
            uint16_t bg_color = (player == JUNQI_PLAYER_RED) ?
                               RGB_TO_RGB565(0xFF, 0x66, 0x66) :
                               RGB_TO_RGB565(0x66, 0x99, 0xFF);
            tft_sprite_fill_rect(&s_sprite, px + 2, py + 2, JUNQI_CELL_SIZE - 4, JUNQI_CELL_SIZE - 4, bg_color);

            // 棋子边框
            tft_sprite_draw_rect(&s_sprite, px + 2, py + 2, JUNQI_CELL_SIZE - 4, JUNQI_CELL_SIZE - 4, TFT_BLACK);

            // 棋子文字
            const char* name = junqi_get_piece_name(piece);
            int text_x = px + JUNQI_CELL_SIZE / 2 - 3;
            int text_y = py + JUNQI_CELL_SIZE / 2 - 4;
            tft_sprite_draw_char(&s_sprite, text_x, text_y, name[0], TFT_BLACK, 1);
        }
    }
}

// 画光标和选中标记
static void junqi_draw_cursor(void)
{
    // 画选中的棋子
    if (s_ui->game.has_selected) {
        int sx = JUNQI_OFFSET_X + s_ui->game.selected_x * JUNQI_CELL_SIZE;
        int sy = JUNQI_OFFSET_Y + s_ui->game.selected_y * JUNQI_CELL_SIZE;
        tft_sprite_draw_rect(&s_sprite, sx, sy, JUNQI_CELL_SIZE - 1, JUNQI_CELL_SIZE - 1, TFT_YELLOW);
        tft_sprite_draw_rect(&s_sprite, sx + 1, sy + 1, JUNQI_CELL_SIZE - 3, JUNQI_CELL_SIZE - 3, TFT_YELLOW);
    }

    // 画光标
    int cx = JUNQI_OFFSET_X + s_ui->cursor_x * JUNQI_CELL_SIZE;
    int cy = JUNQI_OFFSET_Y + s_ui->cursor_y * JUNQI_CELL_SIZE;
    uint16_t cursor_color = (s_ui->game.current_player == JUNQI_PLAYER_RED) ?
                           RGB_TO_RGB565(0xFF, 0x00, 0x00) :
                           RGB_TO_RGB565(0x00, 0x00, 0xFF);

    // 四角标记
    int s = 4;
    tft_sprite_fill_rect(&s_sprite, cx, cy, s, s, cursor_color);
    tft_sprite_fill_rect(&s_sprite, cx + JUNQI_CELL_SIZE - s, cy, s, s, cursor_color);
    tft_sprite_fill_rect(&s_sprite, cx, cy + JUNQI_CELL_SIZE - s, s, s, cursor_color);
    tft_sprite_fill_rect(&s_sprite, cx + JUNQI_CELL_SIZE - s, cy + JUNQI_CELL_SIZE - s, s, s, cursor_color);
}

// 画信息栏
static void junqi_draw_info(void)
{
    // 当前玩家
    const char* turn_text = (s_ui->game.current_player == JUNQI_PLAYER_RED) ?
                           "Turn: Red" : "Turn: Blue";
    int x = 5;
    int y = TFT_HEIGHT - 10;
    for (const char* p = turn_text; *p; p++) {
        tft_sprite_draw_char(&s_sprite, x, y, *p, TFT_LIGHTGREY, 1);
        x += 6;
    }

    // 提示
    const char* hint = "A:Sel/Mov B:Back";
    x = 80;
    for (const char* p = hint; *p; p++) {
        tft_sprite_draw_char(&s_sprite, x, y, *p, TFT_LIGHTGREY, 1);
        x += 6;
    }
}

static void junqi_render_internal(void)
{
    junqi_draw_board();
    junqi_draw_pieces();
    junqi_draw_cursor();
    junqi_draw_info();

    // 游戏结束提示
    if (s_ui->game.game_over) {
        const char* result = (s_ui->game.winner == JUNQI_PLAYER_RED) ?
                            "Red Wins!" : "Blue Wins!";
        int x = (TFT_WIDTH - tft_sprite_text_width(result, 1)) / 2;
        int y = 50;
        tft_sprite_fill_rect(&s_sprite, x - 5, y - 5, tft_sprite_text_width(result, 1) + 10, 18, COLOR_DIALOG);
        for (const char* p = result; *p; p++) {
            tft_sprite_draw_char(&s_sprite, x, y, *p, TFT_YELLOW, 1);
            x += 6;
        }

        const char* hint_text = "A/B:Back";
        x = (TFT_WIDTH - tft_sprite_text_width(hint_text, 1)) / 2;
        y = 70;
        for (const char* p = hint_text; *p; p++) {
            tft_sprite_draw_char(&s_sprite, x, y, *p, TFT_LIGHTGREY, 1);
            x += 6;
        }
    }

    // 消息提示
    if (s_ui->show_message) {
        int x = (TFT_WIDTH - tft_sprite_text_width(s_ui->message_text, 1)) / 2;
        int y = 50;
        tft_sprite_fill_rect(&s_sprite, x - 5, y - 5, tft_sprite_text_width(s_ui->message_text, 1) + 10, 18, COLOR_DIALOG);
        for (const char* p = s_ui->message_text; *p; p++) {
            tft_sprite_draw_char(&s_sprite, x, y, *p, TFT_WHITE, 1);
            x += 6;
        }
    }
}

void junqi_ui_render(void)
{
    if (!s_ui || !s_sprite_created) return;

    int64_t now = esp_timer_get_time();
    if (now - s_last_render < 33000) return;
    s_last_render = now;

    ui_lock_acquire();
    junqi_render_internal();
    tft_sprite_push(&s_sprite, 0, 0);
    ui_lock_release();
}

void junqi_ui_handle_key(int key)
{
    if (!s_ui) return;

    ui_lock_acquire();

    if (s_ui->game.game_over) {
        if (key == KEY_B || key == KEY_A) {
            sound_play(SOUND_KEY_CLICK);
            s_should_exit = true;
        }
        ui_lock_release();
        return;
    }

    if (key == KEY_UP) {
        if (s_ui->cursor_y > 0) {
            s_ui->cursor_y--;
            sound_play(SOUND_KEY_CLICK);
        }
    } else if (key == KEY_DOWN) {
        if (s_ui->cursor_y < JUNQI_BOARD_HEIGHT - 1) {
            s_ui->cursor_y++;
            sound_play(SOUND_KEY_CLICK);
        }
    } else if (key == KEY_LEFT) {
        if (s_ui->cursor_x > 0) {
            s_ui->cursor_x--;
            sound_play(SOUND_KEY_CLICK);
        }
    } else if (key == KEY_RIGHT) {
        if (s_ui->cursor_x < JUNQI_BOARD_WIDTH - 1) {
            s_ui->cursor_x++;
            sound_play(SOUND_KEY_CLICK);
        }
    } else if (key == KEY_A) {
        if (s_ui->game.has_selected) {
            // 尝试移动
            if (s_ui->game.selected_x == s_ui->cursor_x &&
                s_ui->game.selected_y == s_ui->cursor_y) {
                // 取消选择
                s_ui->game.has_selected = false;
                sound_play(SOUND_KEY_CLICK);
            } else {
                // 移动棋子
                if (junqi_make_move(&s_ui->game,
                                   s_ui->game.selected_x, s_ui->game.selected_y,
                                   s_ui->cursor_x, s_ui->cursor_y)) {
                    s_ui->game.has_selected = false;
                    sound_play(SOUND_MOVE);
                } else {
                    sound_play(SOUND_INVALID_MOVE);
                }
            }
        } else {
            // 选择棋子
            JunqiPiece piece = s_ui->game.board[s_ui->cursor_y][s_ui->cursor_x];
            if (piece != JUNQI_PIECE_NONE &&
                junqi_get_piece_player(piece) == s_ui->game.current_player) {
                s_ui->game.selected_x = s_ui->cursor_x;
                s_ui->game.selected_y = s_ui->cursor_y;
                s_ui->game.has_selected = true;
                sound_play(SOUND_KEY_CLICK);
            } else {
                sound_play(SOUND_INVALID_MOVE);
            }
        }
    } else if (key == KEY_B) {
        if (s_ui->game.has_selected) {
            // 取消选择
            s_ui->game.has_selected = false;
            sound_play(SOUND_KEY_CLICK);
        } else {
            // 返回菜单
            sound_play(SOUND_KEY_CLICK);
            s_should_exit = true;
        }
    }

    ui_lock_release();
}

void junqi_ui_update(void)
{
    if (!s_ui) return;

    if (s_ui->show_message) {
        if (s_ui->message_timer > 0) {
            s_ui->message_timer--;
        } else {
            s_ui->show_message = false;
        }
    }
}
