#include "xiangqi_ui.h"
#include "xiangqi_pieces.h"
#include "sound.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bluetooth_game.h"
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

    // 最后一步高亮
    int last_move_from_x;
    int last_move_from_y;
    int last_move_to_x;
    int last_move_to_y;
    bool has_last_move;

    // 子力价值
    int red_material;
    int black_material;

    // 计时器
    int64_t red_elapsed_us;
    int64_t black_elapsed_us;
    int64_t turn_start_time;
    XiangqiColor current_turn_tracked;

    // Bluetooth state
    bool bluetooth_initialized;
    int selected_device_index;
    char bluetooth_status[64];

    bool show_message;
    char message_text[64];
    int message_timer;

    int64_t splash_start_time;
    int64_t last_render_tick;
} XiangqiUIState;

static XiangqiUIState* g_ui = NULL;
static TFT_Sprite g_sprite;
static bool g_sprite_created = false;
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

// 获取棋子的子力价值
static int get_piece_value(XiangqiPieceType type) {
    switch(type) {
        case XIANGQI_PIECE_PAWN:
        case XIANGQI_PIECE_PAWN_B:
            return 1;
        case XIANGQI_PIECE_ADVISOR:
        case XIANGQI_PIECE_ADVISOR_B:
            return 2;
        case XIANGQI_PIECE_ELEPHANT:
        case XIANGQI_PIECE_ELEPHANT_B:
            return 2;
        case XIANGQI_PIECE_HORSE:
        case XIANGQI_PIECE_HORSE_B:
            return 4;
        case XIANGQI_PIECE_CHARIOT:
        case XIANGQI_PIECE_CHARIOT_B:
            return 9;
        case XIANGQI_PIECE_CANNON:
        case XIANGQI_PIECE_CANNON_B:
            return 4;
        case XIANGQI_PIECE_KING:
        case XIANGQI_PIECE_KING_B:
            return 1000;
        default:
            return 0;
    }
}

// 重新计算子力价值
static void recalculate_material(void) {
    g_ui->red_material = 0;
    g_ui->black_material = 0;
    for (int y = 0; y < XIANGQI_BOARD_HEIGHT; y++) {
        for (int x = 0; x < XIANGQI_BOARD_WIDTH; x++) {
            XiangqiPiece piece = g_ui->game.board[y][x];
            if (piece.type != XIANGQI_PIECE_NONE) {
                int value = get_piece_value(piece.type);
                XiangqiColor color = xiangqi_get_piece_color(piece);
                if (color == XIANGQI_COLOR_RED) {
                    g_ui->red_material += value;
                } else {
                    g_ui->black_material += value;
                }
            }
        }
    }
}

// 声明外部函数
extern void request_ui_render(void);

// Bluetooth callbacks
static void on_bluetooth_state_change(BluetoothGameState state) {
    ESP_LOGI(TAG, "Bluetooth state changed: %d", state);
    request_ui_render();
}

static void on_device_found(const FoundBluetoothDevice* device, int index) {
    ESP_LOGI(TAG, "Device found: %s", device->name);
    request_ui_render();
}

static void on_device_list_complete(int count) {
    ESP_LOGI(TAG, "Device list complete: %d", count);
    request_ui_render();
}

static void on_connected(void) {
    ESP_LOGI(TAG, "Bluetooth connected");
    request_ui_render();
}

static void on_disconnected(void) {
    ESP_LOGI(TAG, "Bluetooth disconnected");
    request_ui_render();
}

static void on_invite_received(void) {
    ESP_LOGI(TAG, "Invite received");
    if (g_ui && g_ui->current_screen == XQ_SCREEN_BLUETOOTH_WAITING) {
        // 主机直接接受连接
        bt_game_accept_connection();
    }
    request_ui_render();
}

static void on_invite_accepted(int local_side) {
    ESP_LOGI(TAG, "Invite accepted, local side: %d", local_side);
    // 切换到游戏界面
    xiangqi_ui_switch_screen(XQ_SCREEN_GAME);
    sound_play(SOUND_GAME_START);
    request_ui_render();
}

static void on_move_received(int from_x, int from_y, int to_x, int to_y, int promotion) {
    ESP_LOGI(TAG, "Move received: %d,%d -> %d,%d", from_x, from_y, to_x, to_y);
    if (!g_ui) return;

    ui_lock_acquire();

    // 获取走棋前的回合
    XiangqiColor moving_color = xiangqi_get_current_turn(&g_ui->game);

    // 更新计时器
    int64_t now = esp_timer_get_time();
    if (moving_color == XIANGQI_COLOR_RED) {
        g_ui->red_elapsed_us += now - g_ui->turn_start_time;
    } else {
        g_ui->black_elapsed_us += now - g_ui->turn_start_time;
    }

    // 执行走棋
    if (xiangqi_make_move(&g_ui->game, from_x, from_y, to_x, to_y)) {
        // 保存游戏状态用于同步
        bt_game_save_game_state((GenericGameState*)&g_ui->game);

        // 更新最后一步高亮
        g_ui->last_move_from_x = from_x;
        g_ui->last_move_from_y = from_y;
        g_ui->last_move_to_x = to_x;
        g_ui->last_move_to_y = to_y;
        g_ui->has_last_move = true;

        // 检查是否吃子并更新子力价值
        XiangqiPiece captured = g_ui->game.last_move.captured_piece;
        if (captured.type != XIANGQI_PIECE_NONE) {
            sound_play(SOUND_CAPTURE);
            // 被吃掉的棋子价值加到对方
            int value = get_piece_value(captured.type);
            if (value > 0 && value < 1000) {  // 不算将/帅
                if (moving_color == XIANGQI_COLOR_RED) {
                    g_ui->red_material += value;
                } else {
                    g_ui->black_material += value;
                }
            }
        } else {
            sound_play(SOUND_MOVE);
        }

        // 检查游戏结束
        if (xiangqi_is_game_over(&g_ui->game)) {
            sound_play(SOUND_CHECKMATE);
        } else {
            // 检查将军
            XiangqiColor current_turn = xiangqi_get_current_turn(&g_ui->game);
            if (xiangqi_is_in_check(&g_ui->game, current_turn)) {
                sound_play(SOUND_CHECK);
            }
        }
    }

    // 启动新回合的计时器
    g_ui->turn_start_time = esp_timer_get_time();
    g_ui->current_turn_tracked = xiangqi_get_current_turn(&g_ui->game);

    xiangqi_ui_render_locked();
    ui_lock_release();
}

static void on_game_over(int winner) {
    ESP_LOGI(TAG, "Game over, winner: %d", winner);
    if (!g_ui) return;

    ui_lock_acquire();
    g_ui->game.game_over = true;
    g_ui->game.winner = (winner == 0) ? XIANGQI_COLOR_RED : XIANGQI_COLOR_BLACK;
    sound_play(SOUND_CHECKMATE);
    xiangqi_ui_render_locked();
    ui_lock_release();
}

static void on_sync_received(const GenericGameState* game) {
    ESP_LOGI(TAG, "Game sync received");
    if (!g_ui || !game) return;

    ui_lock_acquire();
    // 注意：这里类型不完全匹配，但我们可以复制基本数据
    // 实际上我们需要一个专门为中国象棋设计的同步
    // 暂时只复制基本状态
    g_ui->game.current_turn = (game->current_turn == 0) ? XIANGQI_COLOR_RED : XIANGQI_COLOR_BLACK;
    g_ui->game.move_count = game->fullmove_number;
    g_ui->game.game_over = game->game_over;
    g_ui->game.winner = (game->winner == 0) ? XIANGQI_COLOR_RED :
                       (game->winner == 1) ? XIANGQI_COLOR_BLACK : XIANGQI_COLOR_NONE;

    // 重新计算子力价值
    recalculate_material();

    // 更新回合跟踪
    g_ui->current_turn_tracked = g_ui->game.current_turn;
    g_ui->turn_start_time = esp_timer_get_time();

    xiangqi_ui_render_locked();
    ui_lock_release();
}

void xiangqi_ui_init(void)
{
    if (g_ui != NULL) {
        return;
    }

    ESP_LOGI(TAG, "Initializing Xiangqi UI");

    // 立即设置为中国象棋游戏类型
    bt_game_set_type(BT_GAME_XIANGQI);

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

    // 初始化最后一步
    g_ui->last_move_from_x = -1;
    g_ui->last_move_from_y = -1;
    g_ui->last_move_to_x = -1;
    g_ui->last_move_to_y = -1;
    g_ui->has_last_move = false;

    // 初始化子力价值和计时器
    g_ui->red_material = 0;
    g_ui->black_material = 0;
    g_ui->red_elapsed_us = 0;
    g_ui->black_elapsed_us = 0;
    g_ui->current_turn_tracked = XIANGQI_COLOR_RED;

    g_ui->bluetooth_initialized = false;
    g_ui->selected_device_index = -1;
    strcpy(g_ui->bluetooth_status, "Bluetooth: Disabled");
    g_ui->splash_start_time = esp_timer_get_time();

    // 计算初始子力价值
    recalculate_material();

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

    // 清理Bluetooth
    if (screen == XQ_SCREEN_MAIN_MENU &&
        (g_ui->current_screen == XQ_SCREEN_BLUETOOTH_SCAN ||
         g_ui->current_screen == XQ_SCREEN_BLUETOOTH_WAITING ||
         g_ui->current_screen == XQ_SCREEN_BLUETOOTH_MENU)) {
        ESP_LOGI(TAG, "Cleaning up Bluetooth on return to main menu");
        bt_game_stop_scanning();
        bt_game_stop_hosting();
    }

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
                // 设置为中国象棋游戏类型
                bt_game_set_type(BT_GAME_XIANGQI);
                bt_game_init();
                // 设置回调
                bt_game_set_state_callback(on_bluetooth_state_change);
                bt_game_set_device_found_callback(on_device_found);
                bt_game_set_device_list_complete_callback(on_device_list_complete);
                bt_game_set_connected_callback(on_connected);
                bt_game_set_disconnected_callback(on_disconnected);
                bt_game_set_invite_callback(on_invite_received);
                bt_game_set_invite_accepted_callback(on_invite_accepted);
                bt_game_set_move_callback(on_move_received);
                bt_game_set_game_over_callback(on_game_over);
                bt_game_set_sync_callback(on_sync_received);
                g_ui->bluetooth_initialized = true;
                strcpy(g_ui->bluetooth_status, "Bluetooth: Ready");
            } else {
                // 确保每次进入蓝牙菜单都设置正确的游戏类型
                bt_game_set_type(BT_GAME_XIANGQI);
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
        {
            // 检查是否从蓝牙重连回来
            BluetoothGameState bt_state = bt_game_get_state();
            bool was_reconnecting = (bt_state == BT_GAME_STATE_GAME_READY ||
                                    bt_state == BT_GAME_STATE_PLAYING ||
                                    bt_state == BT_GAME_STATE_RECONNECTING);
            bool was_in_exit_confirm = (g_ui->previous_screen == XQ_SCREEN_EXIT_CONFIRM);

            if (!was_reconnecting && !was_in_exit_confirm) {
                // 新游戏才重置
                xiangqi_game_reset(&g_ui->game);
                g_ui->cursor_x = 4;
                g_ui->cursor_y = 9;
                g_ui->selected_x = -1;
                g_ui->selected_y = -1;
                g_ui->has_selection = false;

                // 重置最后一步、子力价值和计时器
                g_ui->last_move_from_x = -1;
                g_ui->last_move_from_y = -1;
                g_ui->last_move_to_x = -1;
                g_ui->last_move_to_y = -1;
                g_ui->has_last_move = false;
                g_ui->red_material = 0;
                g_ui->black_material = 0;
                g_ui->red_elapsed_us = 0;
                g_ui->black_elapsed_us = 0;
                g_ui->current_turn_tracked = XIANGQI_COLOR_RED;
                g_ui->turn_start_time = esp_timer_get_time();

                // 重新计算初始子力价值
                recalculate_material();
            }
            break;
        }
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
    // 背景渐变
    for (int y = 0; y < TFT_HEIGHT; y++) {
        uint8_t r = 0x10 + (y * 0x20 / TFT_HEIGHT);
        uint8_t g = 0x10 + (y * 0x18 / TFT_HEIGHT);
        uint8_t b = 0x08 + (y * 0x10 / TFT_HEIGHT);
        uint16_t color = RGB_TO_RGB565(r, g, b);
        tft_sprite_draw_hline(&g_sprite, 0, y, TFT_WIDTH, color);
    }

    // 标题
    const char* title = "XIANGQI";
    int title_width = tft_sprite_text_width(title, 2);
    int title_x = (TFT_WIDTH - title_width) / 2;
    int title_y = 35;

    // 绘制阴影
    tft_sprite_draw_string(&g_sprite, title_x - 1, title_y - 1, title, COLOR_BLACK, 2);
    tft_sprite_draw_string(&g_sprite, title_x + 1, title_y - 1, title, COLOR_BLACK, 2);
    tft_sprite_draw_string(&g_sprite, title_x - 1, title_y + 1, title, COLOR_BLACK, 2);
    tft_sprite_draw_string(&g_sprite, title_x + 1, title_y + 1, title, COLOR_BLACK, 2);

    // 绘制主标题
    tft_sprite_draw_string(&g_sprite, title_x, title_y, title, COLOR_GOLD, 2);

    // 副标题
    const char* subtitle = "Chinese Chess";
    int subtitle_x = (TFT_WIDTH - tft_sprite_text_width(subtitle, 1)) / 2;
    tft_sprite_draw_string(&g_sprite, subtitle_x, 70, subtitle, COLOR_YELLOW, 1);
}

static void render_main_menu(void)
{
    // 标题
    int title_x = (TFT_WIDTH - tft_sprite_text_width("XIANGQI", 2)) / 2;
    tft_sprite_draw_string(&g_sprite, title_x, 4, "XIANGQI", COLOR_WHITE, 2);

    // 菜单项
    for (int i = 0; i < 3; i++) {
        int y = 35 + i * 22;
        bool is_selected = (i == g_ui->menu_index);

        if (is_selected) {
            tft_sprite_fill_rect(&g_sprite, 20, y, 120, 20, COLOR_NAVY);
        }

        tft_sprite_draw_string(&g_sprite, 25, y + 6, main_menu_items[i],
                              is_selected ? COLOR_YELLOW : COLOR_WHITE, 1);
    }
}

static void render_bluetooth_menu(void)
{
    // 标题
    const char* title = "Bluetooth Game";
    int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 1)) / 2;
    tft_sprite_draw_string(&g_sprite, title_x, 5, title, COLOR_WHITE, 1);

    // 状态
    tft_sprite_draw_string(&g_sprite, 10, 20, g_ui->bluetooth_status, COLOR_LIGHT_GRAY, 1);

    // 菜单项
    for (int i = 0; i < 3; i++) {
        int y = 40 + i * 25;
        bool is_selected = (i == g_ui->menu_index);

        if (is_selected) {
            tft_sprite_fill_rect(&g_sprite, 20, y, 120, 20, COLOR_NAVY);
        }

        tft_sprite_draw_string(&g_sprite, 25, y + 6, bluetooth_menu_items[i],
                              is_selected ? COLOR_YELLOW : COLOR_WHITE, 1);
    }
}

static void render_bluetooth_scan(void)
{
    // 标题
    const char* title = "Scan Results";
    int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 1)) / 2;
    tft_sprite_draw_string(&g_sprite, title_x, 5, title, COLOR_WHITE, 1);

    // 获取设备列表
    FoundBluetoothDevice local_devices[MAX_FOUND_DEVICES];
    int device_count = bt_game_copy_found_devices(local_devices, MAX_FOUND_DEVICES);

    if (device_count == 0) {
        tft_sprite_draw_string(&g_sprite, 10, 25, "Searching...", COLOR_YELLOW, 1);
    } else {
        int display_count = (device_count < 4) ? device_count : 4;
        for (int i = 0; i < display_count; i++) {
            FoundBluetoothDevice* device = &local_devices[i];
            int y = 25 + i * 25;
            bool is_selected = (i == g_ui->selected_device_index);

            if (is_selected) {
                tft_sprite_fill_rect(&g_sprite, 5, y, TFT_WIDTH - 10, 20, COLOR_NAVY);
            }

            tft_sprite_draw_string(&g_sprite, 10, y + 6, device->name,
                                  is_selected ? COLOR_YELLOW : COLOR_WHITE, 1);
        }
    }

    // 提示
    tft_sprite_draw_string(&g_sprite, 10, TFT_HEIGHT - 12, "B: Back", COLOR_LIGHT_GRAY, 1);
}

static void render_bluetooth_waiting(void)
{
    // 标题
    const char* title = "Waiting...";
    int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 1)) / 2;
    tft_sprite_draw_string(&g_sprite, title_x, 40, title, COLOR_WHITE, 1);

    // 状态
    tft_sprite_draw_string(&g_sprite, 10, 60, g_ui->bluetooth_status, COLOR_YELLOW, 1);

    // 提示
    tft_sprite_draw_string(&g_sprite, 10, TFT_HEIGHT - 12, "B: Cancel", COLOR_LIGHT_GRAY, 1);
}

static void xiangqi_draw_board_lines(void)
{
    // 棋盘背景 - 调暗一点
    uint16_t board_bg = RGB_TO_RGB565(0x8B, 0x5A, 0x2B);
    tft_sprite_fill_rect(&g_sprite, XIANGQI_OFFSET_X, XIANGQI_OFFSET_Y,
                        XIANGQI_BOARD_WIDTH * XIANGQI_SQUARE_SIZE,
                        XIANGQI_BOARD_HEIGHT * XIANGQI_SQUARE_SIZE,
                        board_bg);

    // 绘制横线
    for (int y = 0; y < XIANGQI_BOARD_HEIGHT; y++) {
        int py = XIANGQI_OFFSET_Y + y * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
        int px1 = XIANGQI_OFFSET_X + XIANGQI_SQUARE_SIZE / 2;
        int px2 = XIANGQI_OFFSET_X + (XIANGQI_BOARD_WIDTH - 1) * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
        tft_sprite_draw_line(&g_sprite, px1, py, px2, py, COLOR_BLACK);
    }

    // 绘制竖线（注意楚河汉界处中断）
    for (int x = 0; x < XIANGQI_BOARD_WIDTH; x++) {
        int px = XIANGQI_OFFSET_X + x * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
        int py1, py2;

        if (x == 0 || x == XIANGQI_BOARD_WIDTH - 1) {
            // 最左和最右竖线贯通
            py1 = XIANGQI_OFFSET_Y + XIANGQI_SQUARE_SIZE / 2;
            py2 = XIANGQI_OFFSET_Y + (XIANGQI_BOARD_HEIGHT - 1) * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
            tft_sprite_draw_line(&g_sprite, px, py1, px, py2, COLOR_BLACK);
        } else {
            // 其他竖线在楚河汉界处中断
            py1 = XIANGQI_OFFSET_Y + XIANGQI_SQUARE_SIZE / 2;
            py2 = XIANGQI_OFFSET_Y + 4 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
            tft_sprite_draw_line(&g_sprite, px, py1, px, py2, COLOR_BLACK);
            py1 = XIANGQI_OFFSET_Y + 5 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
            py2 = XIANGQI_OFFSET_Y + 9 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
            tft_sprite_draw_line(&g_sprite, px, py1, px, py2, COLOR_BLACK);
        }
    }

    // 绘制九宫格斜线（红方）
    tft_sprite_draw_line(&g_sprite,
        XIANGQI_OFFSET_X + 3 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_Y + 7 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_X + 5 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_Y + 9 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        COLOR_BLACK);
    tft_sprite_draw_line(&g_sprite,
        XIANGQI_OFFSET_X + 5 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_Y + 7 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_X + 3 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_Y + 9 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        COLOR_BLACK);

    // 绘制九宫格斜线（黑方）
    tft_sprite_draw_line(&g_sprite,
        XIANGQI_OFFSET_X + 3 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_Y + 0 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_X + 5 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_Y + 2 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        COLOR_BLACK);
    tft_sprite_draw_line(&g_sprite,
        XIANGQI_OFFSET_X + 5 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_Y + 0 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_X + 3 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        XIANGQI_OFFSET_Y + 2 * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2,
        COLOR_BLACK);
}

static void xiangqi_draw_piece(int x, int y, XiangqiPiece piece)
{
    if (piece.type == XIANGQI_PIECE_NONE) return;

    const uint8_t* bitmap = xiangqi_get_piece_bitmap(piece.type, xiangqi_get_piece_color(piece));
    if (!bitmap) return;

    // 计算圆心位置
    int cx = XIANGQI_OFFSET_X + x * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;
    int cy = XIANGQI_OFFSET_Y + y * XIANGQI_SQUARE_SIZE + XIANGQI_SQUARE_SIZE / 2;

    XiangqiColor color = xiangqi_get_piece_color(piece);
    uint16_t text_color = (color == XIANGQI_COLOR_RED) ? COLOR_RED : COLOR_BLACK;

    // 计算位图左上角位置，确保居中
    int bitmap_x = cx - XIANGQI_PIECE_WIDTH / 2;
    int bitmap_y = cy - XIANGQI_PIECE_HEIGHT / 2;

    // 绘制棋子位图
    for (int dy = 0; dy < XIANGQI_PIECE_HEIGHT; dy++) {
        for (int dx = 0; dx < XIANGQI_PIECE_WIDTH; dx++) {
            int byte_idx = dy * 2 + (dx / 8);
            int bit_idx = 7 - (dx % 8);
            if (bitmap[byte_idx] & (1 << bit_idx)) {
                tft_sprite_draw_pixel(&g_sprite, bitmap_x + dx, bitmap_y + dy, text_color);
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
    uint16_t cursor_color = RGB_TO_RGB565(0xFF, 0xFF, 0x00); // 亮黄色更醒目
    tft_sprite_fill_rect(&g_sprite, px, py, 5, 5, cursor_color);
    tft_sprite_fill_rect(&g_sprite, px + s - 5, py, 5, 5, cursor_color);
    tft_sprite_fill_rect(&g_sprite, px, py + s - 5, 5, 5, cursor_color);
    tft_sprite_fill_rect(&g_sprite, px + s - 5, py + s - 5, 5, 5, cursor_color);
}

static void xiangqi_draw_info(void)
{
    int info_x = XIANGQI_OFFSET_X + XIANGQI_BOARD_WIDTH * XIANGQI_SQUARE_SIZE + 4;
    int y = 2;

    // ========== 顶部：蓝牙状态和回合指示 ==========
    BluetoothGameState bt_state = bt_game_get_state();
    bool is_bluetooth_connected = (bt_state >= BT_GAME_STATE_CONNECTED && bt_state != BT_GAME_STATE_RECONNECTING);
    uint16_t bt_status_color = (bt_state == BT_GAME_STATE_RECONNECTING) ? COLOR_YELLOW :
                              (is_bluetooth_connected ? COLOR_GREEN : COLOR_RED);
    // 蓝牙状态指示灯
    tft_sprite_fill_rect(&g_sprite, info_x, y, 8, 8, bt_status_color);
    tft_sprite_draw_rect(&g_sprite, info_x, y, 8, 8, COLOR_LIGHT_GRAY);
    y += 12;

    // ========== 回合指示 ==========
    XiangqiColor turn = xiangqi_get_current_turn(&g_ui->game);
    tft_sprite_draw_string(&g_sprite, info_x, y, "TURN", COLOR_LIGHT_GRAY, 1);
    y += 10;
    // 回合颜色方块
    tft_sprite_fill_rect(&g_sprite, info_x, y, 16, 12,
                        (turn == XIANGQI_COLOR_RED) ? COLOR_RED : COLOR_BLACK);
    tft_sprite_draw_rect(&g_sprite, info_x, y, 16, 12, COLOR_LIGHT_GRAY);
    // 回合文字
    tft_sprite_draw_string(&g_sprite, info_x + 20, y + 2,
                          (turn == XIANGQI_COLOR_RED) ? "RED" : "BLK",
                          (turn == XIANGQI_COLOR_RED) ? COLOR_RED : COLOR_WHITE, 1);
    y += 16;

    // 将军指示
    if (xiangqi_is_in_check(&g_ui->game, turn)) {
        tft_sprite_draw_string(&g_sprite, info_x, y, "CHECK!", COLOR_YELLOW, 1);
        y += 12;
    }

    // 分隔线
    y += 4;

    // ========== 红方信息 ==========
    tft_sprite_draw_string(&g_sprite, info_x, y, "RED", COLOR_RED, 1);
    y += 10;
    // 子力价值
    char red_mat_text[16];
    snprintf(red_mat_text, sizeof(red_mat_text), "MAT:%d", g_ui->red_material);
    tft_sprite_draw_string(&g_sprite, info_x, y, red_mat_text, COLOR_LIGHT_GRAY, 1);
    y += 10;

    // ========== 计时器计算 ==========
    int64_t now = esp_timer_get_time();
    int64_t red_total = g_ui->red_elapsed_us;
    int64_t black_total = g_ui->black_elapsed_us;

    // 加上当前回合的时间
    if (turn == XIANGQI_COLOR_RED) {
        int64_t elapsed = now - g_ui->turn_start_time;
        red_total += (elapsed > 0) ? elapsed : 0;
    } else {
        int64_t elapsed = now - g_ui->turn_start_time;
        black_total += (elapsed > 0) ? elapsed : 0;
    }

    // 确保非负
    if (red_total < 0) red_total = 0;
    if (black_total < 0) black_total = 0;

    // 红方计时器
    int red_sec = (int)(red_total / 1000000);
    int red_min = red_sec / 60;
    red_sec = red_sec % 60;
    if (red_min < 0) red_min = 0;
    if (red_min > 99) red_min = 99;
    if (red_sec < 0) red_sec = 0;
    if (red_sec > 59) red_sec = 59;
    char red_timer_text[16];
    snprintf(red_timer_text, sizeof(red_timer_text), "%02d:%02d", red_min, red_sec);
    tft_sprite_draw_string(&g_sprite, info_x, y, red_timer_text,
                          turn == XIANGQI_COLOR_RED ? COLOR_YELLOW : COLOR_LIGHT_GRAY, 1);
    y += 14;

    // 分隔线
    tft_sprite_draw_hline(&g_sprite, info_x, y, 30, COLOR_LIGHT_GRAY);
    y += 6;

    // ========== 黑方信息 ==========
    tft_sprite_draw_string(&g_sprite, info_x, y, "BLK", COLOR_WHITE, 1);
    y += 10;
    // 子力价值
    char black_mat_text[16];
    snprintf(black_mat_text, sizeof(black_mat_text), "MAT:%d", g_ui->black_material);
    tft_sprite_draw_string(&g_sprite, info_x, y, black_mat_text, COLOR_LIGHT_GRAY, 1);
    y += 10;

    // 黑方计时器
    int black_sec = (int)(black_total / 1000000);
    int black_min = black_sec / 60;
    black_sec = black_sec % 60;
    if (black_min < 0) black_min = 0;
    if (black_min > 99) black_min = 99;
    if (black_sec < 0) black_sec = 0;
    if (black_sec > 59) black_sec = 59;
    char black_timer_text[16];
    snprintf(black_timer_text, sizeof(black_timer_text), "%02d:%02d", black_min, black_sec);
    tft_sprite_draw_string(&g_sprite, info_x, y, black_timer_text,
                          turn == XIANGQI_COLOR_BLACK ? COLOR_YELLOW : COLOR_LIGHT_GRAY, 1);
    y += 14;

    // 分隔线
    tft_sprite_draw_hline(&g_sprite, info_x, y, 30, COLOR_LIGHT_GRAY);
    y += 6;

    // ========== 步数 ==========
    char move_text[16];
    snprintf(move_text, sizeof(move_text), "MOVE:%d", g_ui->game.move_count);
    tft_sprite_draw_string(&g_sprite, info_x, y, move_text, COLOR_LIGHT_GRAY, 1);
    y += 12;

    // ========== 蓝牙游戏回合提示 ==========
    if (bt_state >= BT_GAME_STATE_GAME_READY) {
        const char* turn_label = bt_game_is_my_turn() ? "YOUR TURN" : "OPP TURN";
        uint16_t turn_color = bt_game_is_my_turn() ? COLOR_GREEN : COLOR_RED;
        tft_sprite_fill_rect(&g_sprite, info_x - 2, y, 34, 12, turn_color);
        tft_sprite_draw_rect(&g_sprite, info_x - 2, y, 34, 12, COLOR_LIGHT_GRAY);
        tft_sprite_draw_string(&g_sprite, info_x, y + 2, turn_label, COLOR_BLACK, 1);
        y += 16;
    }

    // ========== 底部：退出提示 ==========
    tft_sprite_draw_string(&g_sprite, info_x, TFT_HEIGHT - 12, "B:EXIT", COLOR_LIGHT_GRAY, 1);
}

static void render_game(void)
{
    xiangqi_draw_board_lines();

    // 绘制棋子
    for (int y = 0; y < XIANGQI_BOARD_HEIGHT; y++) {
        for (int x = 0; x < XIANGQI_BOARD_WIDTH; x++) {
            // 检查是否是最后一步的位置
            bool is_last_move_from = (g_ui->has_last_move && x == g_ui->last_move_from_x && y == g_ui->last_move_from_y);
            bool is_last_move_to = (g_ui->has_last_move && x == g_ui->last_move_to_x && y == g_ui->last_move_to_y);
            bool is_last_move = is_last_move_from || is_last_move_to;

            // 如果是最后一步，高亮背景
            if (is_last_move) {
                int px = XIANGQI_OFFSET_X + x * XIANGQI_SQUARE_SIZE;
                int py = XIANGQI_OFFSET_Y + y * XIANGQI_SQUARE_SIZE;
                tft_sprite_fill_rect(&g_sprite, px, py, XIANGQI_SQUARE_SIZE, XIANGQI_SQUARE_SIZE, COLOR_LAST_MOVE);
            }

            xiangqi_draw_piece(x, y, g_ui->game.board[y][x]);
        }
    }

    // 绘制选中高亮
    if (g_ui->has_selection) {
        xiangqi_draw_selection(g_ui->selected_x, g_ui->selected_y, COLOR_GREEN);
    }

    // 绘制光标
    xiangqi_draw_cursor(g_ui->cursor_x, g_ui->cursor_y);

    // 绘制信息面板
    xiangqi_draw_info();

    // 游戏结束提示
    if (g_ui->game.game_over) {
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
        tft_sprite_draw_string(&g_sprite, result_x, result_y, result, COLOR_YELLOW, 1);

        const char* hint = "B: Menu";
        int hint_x = (TFT_WIDTH - tft_sprite_text_width(hint, 1)) / 2;
        tft_sprite_draw_string(&g_sprite, hint_x, result_y + 16, hint, COLOR_LIGHT_GRAY, 1);
    }

    // 重连提示
    BluetoothGameState bt_state = bt_game_get_state();
    if (bt_state == BT_GAME_STATE_RECONNECTING) {
        tft_sprite_fill_rect(&g_sprite, 10, 20, 140, 65, COLOR_DIALOG);
        tft_sprite_draw_rect(&g_sprite, 10, 20, 140, 65, COLOR_YELLOW);

        const char* recon_title = "Connection Lost!";
        int recon_title_x = (TFT_WIDTH - tft_sprite_text_width(recon_title, 1)) / 2;
        tft_sprite_draw_string(&g_sprite, recon_title_x, 35, recon_title, COLOR_YELLOW, 1);

        const char* recon_hint;
        if (bt_game_get_role() == BT_GAME_ROLE_HOST) {
            recon_hint = "Waiting for guest";
        } else {
            recon_hint = "Press A to retry";
        }
        int recon_hint_x = (TFT_WIDTH - tft_sprite_text_width(recon_hint, 1)) / 2;
        tft_sprite_draw_string(&g_sprite, recon_hint_x, 50, recon_hint, COLOR_LIGHT_GRAY, 1);

        const char* cancel_hint = "Press B to exit";
        int cancel_hint_x = (TFT_WIDTH - tft_sprite_text_width(cancel_hint, 1)) / 2;
        tft_sprite_draw_string(&g_sprite, cancel_hint_x, 65, cancel_hint, COLOR_LIGHT_GRAY, 1);
    }

    // 消息提示
    if (g_ui->show_message) {
        int msg_x = (TFT_WIDTH - tft_sprite_text_width(g_ui->message_text, 1)) / 2;
        int msg_y = 50;
        tft_sprite_fill_rect(&g_sprite, msg_x - 4, msg_y - 4, tft_sprite_text_width(g_ui->message_text, 1) + 8, 16, COLOR_DIALOG);
        tft_sprite_draw_string(&g_sprite, msg_x, msg_y, g_ui->message_text, COLOR_WHITE, 1);
    }
}

static void render_exit_confirm(void)
{
    render_game();

    // 绘制确认对话框
    tft_sprite_fill_rect(&g_sprite, 20, 30, 120, 70, COLOR_DIALOG);
    tft_sprite_draw_rect(&g_sprite, 20, 30, 120, 70, COLOR_WHITE);

    const char* title = "Exit Game?";
    int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 1)) / 2;
    tft_sprite_draw_string(&g_sprite, title_x, 45, title, COLOR_WHITE, 1);

    // 菜单项
    for (int i = 0; i < 2; i++) {
        int y = 65 + i * 20;
        bool is_selected = (i == g_ui->menu_index);
        if (is_selected) {
            tft_sprite_fill_rect(&g_sprite, 35, y, 90, 18, COLOR_NAVY);
        }
        tft_sprite_draw_string(&g_sprite, 40, y + 5, exit_confirm_items[i],
                              is_selected ? COLOR_YELLOW : COLOR_WHITE, 1);
    }
}

static void render_internal(bool force)
{
    if (!g_ui || !g_sprite_created) return;

    // 节流控制
    int64_t now = esp_timer_get_time();
    if (!force && now - g_ui->last_render_tick < 33000) {
        return;
    }
    g_ui->last_render_tick = now;

    // 清屏
    tft_sprite_fill_screen(&g_sprite, COLOR_BACKGROUND);

    // 根据当前屏幕渲染
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

    // 推送到屏幕
    tft_sprite_push(&g_sprite, 0, 0);
}

void xiangqi_ui_render_locked(void)
{
    render_internal(false);
}

void xiangqi_ui_render(void)
{
    if (!g_ui || !g_sprite_created) return;

    ui_lock_acquire();
    xiangqi_ui_render_locked();
    ui_lock_release();
}

static void xiangqi_show_message_internal(const char* text)
{
    strncpy(g_ui->message_text, text, sizeof(g_ui->message_text) - 1);
    g_ui->message_text[sizeof(g_ui->message_text) - 1] = '\0';
    g_ui->show_message = true;
    g_ui->message_timer = 20;
}

void xiangqi_ui_show_message(const char* text)
{
    if (!g_ui || !text) return;

    ui_lock_acquire();
    xiangqi_show_message_internal(text);
    xiangqi_ui_render_locked();
    ui_lock_release();
}

void xiangqi_ui_handle_key(int key)
{
    if (!g_ui) return;

    ui_lock_acquire();

    switch (g_ui->current_screen) {
        case XQ_SCREEN_SPLASH:
            // 任意键进入主菜单
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
                    if (bt_game_start_hosting()) {
                        strcpy(g_ui->bluetooth_status, "Waiting for opponent...");
                        xiangqi_ui_switch_screen(XQ_SCREEN_BLUETOOTH_WAITING);
                    }
                } else if (g_ui->menu_index == 1) {
                    if (bt_game_start_scanning()) {
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
                int device_count = bt_game_get_found_device_count();
                if (g_ui->selected_device_index < device_count - 1) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->selected_device_index++;
                }
            } else if (key == KEY_A) {
                if (g_ui->selected_device_index >= 0) {
                    sound_play(SOUND_MENU_SELECT);
                    if (bt_game_connect_to_device(g_ui->selected_device_index)) {
                        strcpy(g_ui->bluetooth_status, "Connecting...");
                        xiangqi_ui_switch_screen(XQ_SCREEN_BLUETOOTH_WAITING);
                    }
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                bt_game_stop_scanning();
                xiangqi_ui_switch_screen(XQ_SCREEN_BLUETOOTH_MENU);
            }
            break;

        case XQ_SCREEN_BLUETOOTH_WAITING:
            if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                bt_game_disconnect();
                bt_game_stop_hosting();
                bt_game_stop_scanning();
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
                    // 确认退出
                    bool is_bluetooth_game = (bt_game_get_state() == BT_GAME_STATE_GAME_READY ||
                                             bt_game_get_state() == BT_GAME_STATE_PLAYING);
                    if (is_bluetooth_game) {
                        bt_game_send_resign();
                    }
                    xiangqi_ui_switch_screen(XQ_SCREEN_MAIN_MENU);
                } else {
                    // 取消
                    xiangqi_ui_switch_screen(g_ui->previous_screen);
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                // 取消
                xiangqi_ui_switch_screen(g_ui->previous_screen);
            }
            break;

        case XQ_SCREEN_GAME:
            // 游戏结束时，按任意键返回菜单
            if (g_ui->game.game_over) {
                if (key == KEY_B || key == KEY_A) {
                    sound_play(SOUND_KEY_CLICK);
                    xiangqi_ui_switch_screen(XQ_SCREEN_MAIN_MENU);
                }
                break;
            }

            // 获取蓝牙状态
            BluetoothGameState bt_state = bt_game_get_state();
            bool is_bluetooth_game = (bt_state == BT_GAME_STATE_GAME_READY ||
                                     bt_state == BT_GAME_STATE_PLAYING);
            bool is_reconnecting = (bt_state == BT_GAME_STATE_RECONNECTING);
            bool is_our_turn = true;
            if (is_bluetooth_game) {
                is_our_turn = bt_game_is_my_turn();
            }

            // 重连状态下的处理
            if (is_reconnecting) {
                if (key == KEY_A) {
                    // 尝试重连
                    if (bt_game_try_reconnect()) {
                        sound_play(SOUND_KEY_CLICK);
                        xiangqi_show_message_internal("Reconnecting...");
                    } else {
                        sound_play(SOUND_INVALID_MOVE);
                        xiangqi_show_message_internal("Reconnect failed");
                    }
                } else if (key == KEY_B) {
                    sound_play(SOUND_KEY_CLICK);
                    // 取消重连
                    bt_game_disconnect();
                    xiangqi_ui_switch_screen(XQ_SCREEN_MAIN_MENU);
                }
                break;
            }

            // 正常游戏处理
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

                // 蓝牙游戏时检查是否轮到自己
                if (!is_our_turn && is_bluetooth_game) {
                    sound_play(SOUND_INVALID_MOVE);
                    xiangqi_show_message_internal("Not your turn!");
                    break;
                }

                if (g_ui->has_selection) {
                    if (g_ui->selected_x == g_ui->cursor_x && g_ui->selected_y == g_ui->cursor_y) {
                        // 取消选择
                        sound_play(SOUND_KEY_CLICK);
                        g_ui->has_selection = false;
                        g_ui->selected_x = -1;
                        g_ui->selected_y = -1;
                    } else if (xiangqi_is_valid_move(&g_ui->game, g_ui->selected_x, g_ui->selected_y,
                                                     g_ui->cursor_x, g_ui->cursor_y)) {
                        // 检查是否吃子
                        XiangqiPiece target_piece = g_ui->game.board[g_ui->cursor_y][g_ui->cursor_x];
                        bool is_capture = (target_piece.type != XIANGQI_PIECE_NONE);

                        // 获取走棋前的回合
                        XiangqiColor moving_color = xiangqi_get_current_turn(&g_ui->game);

                        // 更新当前玩家的计时器
                        int64_t now = esp_timer_get_time();
                        if (moving_color == XIANGQI_COLOR_RED) {
                            g_ui->red_elapsed_us += now - g_ui->turn_start_time;
                        } else {
                            g_ui->black_elapsed_us += now - g_ui->turn_start_time;
                        }

                        // 执行走棋
                        if (xiangqi_make_move(&g_ui->game, g_ui->selected_x, g_ui->selected_y,
                                             g_ui->cursor_x, g_ui->cursor_y)) {
                            // 更新最后一步高亮
                            g_ui->last_move_from_x = g_ui->selected_x;
                            g_ui->last_move_from_y = g_ui->selected_y;
                            g_ui->last_move_to_x = g_ui->cursor_x;
                            g_ui->last_move_to_y = g_ui->cursor_y;
                            g_ui->has_last_move = true;

                            if (is_capture) {
                                sound_play(SOUND_CAPTURE);
                                // 更新子力价值
                                int value = get_piece_value(target_piece.type);
                                if (value > 0 && value < 1000) {  // 不算将/帅
                                    if (moving_color == XIANGQI_COLOR_RED) {
                                        g_ui->red_material += value;
                                    } else {
                                        g_ui->black_material += value;
                                    }
                                }
                            } else {
                                sound_play(SOUND_MOVE);
                            }

                            // 检查游戏结束
                            if (xiangqi_is_game_over(&g_ui->game)) {
                                sound_play(SOUND_CHECKMATE);
                                // 蓝牙游戏时发送结束消息
                                if (is_bluetooth_game) {
                                    int winner = (g_ui->game.winner == XIANGQI_COLOR_RED) ? 0 : 1;
                                    bt_game_send_game_over(winner);
                                }
                            } else {
                                // 检查将军
                                XiangqiColor current_turn = xiangqi_get_current_turn(&g_ui->game);
                                if (xiangqi_is_in_check(&g_ui->game, current_turn)) {
                                    sound_play(SOUND_CHECK);
                                }
                            }

                            // 蓝牙游戏时发送走棋
                            if (is_bluetooth_game) {
                                bt_game_save_game_state((GenericGameState*)&g_ui->game);
                                bt_game_send_move(g_ui->selected_x, g_ui->selected_y,
                                                  g_ui->cursor_x, g_ui->cursor_y, 0);
                            }
                        } else {
                            sound_play(SOUND_INVALID_MOVE);
                        }
                        g_ui->has_selection = false;
                        g_ui->selected_x = -1;
                        g_ui->selected_y = -1;

                        // 启动新回合的计时器
                        g_ui->turn_start_time = esp_timer_get_time();
                        g_ui->current_turn_tracked = xiangqi_get_current_turn(&g_ui->game);
                    } else if (piece.type != XIANGQI_PIECE_NONE &&
                              xiangqi_get_piece_color(piece) == xiangqi_get_current_turn(&g_ui->game)) {
                        // 选择另一个自己的棋子
                        sound_play(SOUND_MENU_SELECT);
                        g_ui->selected_x = g_ui->cursor_x;
                        g_ui->selected_y = g_ui->cursor_y;
                    } else {
                        // 无效走法
                        sound_play(SOUND_INVALID_MOVE);
                        g_ui->has_selection = false;
                        g_ui->selected_x = -1;
                        g_ui->selected_y = -1;
                    }
                } else {
                    if (piece.type != XIANGQI_PIECE_NONE &&
                        xiangqi_get_piece_color(piece) == xiangqi_get_current_turn(&g_ui->game)) {
                        // 选择棋子
                        sound_play(SOUND_MENU_SELECT);
                        g_ui->selected_x = g_ui->cursor_x;
                        g_ui->selected_y = g_ui->cursor_y;
                        g_ui->has_selection = true;
                    } else if (piece.type != XIANGQI_PIECE_NONE) {
                        // 选择了对方的棋子
                        sound_play(SOUND_INVALID_MOVE);
                    } else {
                        // 选择了空位
                        sound_play(SOUND_INVALID_MOVE);
                    }
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                // 检查是否在重连状态
                BluetoothGameState state = bt_game_get_state();
                if (state == BT_GAME_STATE_RECONNECTING) {
                    bt_game_disconnect();
                    xiangqi_ui_switch_screen(XQ_SCREEN_MAIN_MENU);
                } else if (g_ui->has_selection) {
                    g_ui->has_selection = false;
                    g_ui->selected_x = -1;
                    g_ui->selected_y = -1;
                } else {
                    // 显示退出确认
                    xiangqi_ui_switch_screen(XQ_SCREEN_EXIT_CONFIRM);
                }
            }
            break;
    }

    xiangqi_ui_render_locked();
    ui_lock_release();
}

void xiangqi_ui_update(void)
{
    if (!g_ui) return;

    // 启动画面自动跳转
    if (g_ui->current_screen == XQ_SCREEN_SPLASH) {
        int64_t now = esp_timer_get_time();
        if (now - g_ui->splash_start_time > 3000000) {
            xiangqi_ui_switch_screen(XQ_SCREEN_MAIN_MENU);
        }
    }

    // 更新消息计时器
    if (g_ui->show_message) {
        if (g_ui->message_timer > 0) {
            g_ui->message_timer--;
        } else {
            g_ui->show_message = false;
        }
    }
}
