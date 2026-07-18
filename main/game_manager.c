#include "game_manager.h"
#include "chess.h"
#include "chess_ui.h"
#include "ui_lock.h"
#include "tft_display.h"
#include "sound.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

// 前向声明
static void game_manager_render_main_menu(void);
static void game_manager_render_game_selector(void);

// 游戏管理器状态
static GameManager* g_mgr = NULL;
static TFT_Sprite g_sprite;
static bool g_sprite_created = false;
static int64_t g_last_render_tick = 0;
static bool g_show_splash = true;
static int64_t g_splash_start_time = 0;

// 游戏选择菜单
static const char* g_game_names[] = {
    "International Chess",
    "Chinese Chess",
    "Gomoku (5 in a row)",
    "Go (9x9)",
};
static const int GAME_COUNT = sizeof(g_game_names) / sizeof(g_game_names[0]);

// 游戏类型枚举（本地使用）
typedef enum {
    MENU_SCREEN_SPLASH,
    MENU_SCREEN_MAIN,
    MENU_SCREEN_GAME_SELECTOR,
    MENU_SCREEN_PLAYING,
} MenuScreen;

static MenuScreen g_current_menu = MENU_SCREEN_SPLASH;
static int g_selected_game_idx = 0;

// 国际象棋适配层 - 复用现有 chess_ui.c 中的逻辑
static void chess_adapter_init(void* game);
static void chess_adapter_reset(void* game);
static bool chess_adapter_select_piece(void* game, int x, int y);
static bool chess_adapter_make_move(void* game, int x1, int y1, int x2, int y2);
static bool chess_adapter_is_valid_move(void* game, int x1, int y1, int x2, int y2);
static int chess_adapter_get_current_player(void* game);
static bool chess_adapter_is_game_over(void* game);
static GameResult chess_adapter_get_result(void* game);
static int chess_adapter_board_width(void);
static int chess_adapter_board_height(void);
static void chess_adapter_render(void* game, TFT_Sprite* sprite, int offset_x, int offset_y,
                                 int cursor_x, int cursor_y, int selected_x, int selected_y);

static const GameInterface g_chess_interface = {
    .type = GAME_TYPE_CHESS,
    .name = "Chess",
    .init = chess_adapter_init,
    .reset = chess_adapter_reset,
    .select_piece = chess_adapter_select_piece,
    .make_move = chess_adapter_make_move,
    .is_valid_move = chess_adapter_is_valid_move,
    .get_current_player = chess_adapter_get_current_player,
    .is_game_over = chess_adapter_is_game_over,
    .get_result = chess_adapter_get_result,
    .get_board_width = chess_adapter_board_width,
    .get_board_height = chess_adapter_board_height,
    .render = chess_adapter_render,
    .game_data_size = sizeof(ChessGame)
};

// 游戏接口列表
static const GameInterface* g_game_interfaces[] = {
    &g_chess_interface,
    NULL, // Chinese Chess (todo)
    NULL, // Gomoku (todo)
    NULL  // Go (todo)
};

// 初始化游戏管理器
void game_manager_init(void)
{
    if (g_mgr != NULL) {
        return; // already initialized
    }

    ESP_LOGI("GM", "Initializing Game Manager");

    g_mgr = (GameManager*)calloc(1, sizeof(GameManager));
    if (!g_mgr) {
        ESP_LOGE("GM", "Failed to allocate Game Manager");
        return;
    }

    // 创建 sprite
    if (tft_sprite_create(&g_sprite, TFT_WIDTH, TFT_HEIGHT) == ESP_OK) {
        g_sprite_created = true;
    }

    // 记录 splash 开始时间
    g_splash_start_time = esp_timer_get_time();
    g_show_splash = true;

    // 初始化菜单状态
    g_mgr->menu_index = 0;
    g_mgr->in_menu = true;
}

// 切换游戏类型
void game_manager_switch_game(GameType type)
{
    // 释放旧游戏数据
    if (g_mgr->game_data != NULL && g_mgr->game_interface != NULL) {
        free(g_mgr->game_data);
    }

    // 查找新游戏接口
    g_mgr->game_interface = NULL;
    for (int i = 0; i < GAME_COUNT; i++) {
        if (g_game_interfaces[i] != NULL && g_game_interfaces[i]->type == type) {
            g_mgr->game_interface = g_game_interfaces[i];
            break;
        }
    }

    if (g_mgr->game_interface != NULL && g_mgr->game_interface->game_data_size > 0) {
        g_mgr->game_data = calloc(1, g_mgr->game_interface->game_data_size);
        if (g_mgr->game_data != NULL) {
            g_mgr->game_interface->init(g_mgr->game_data);
        }
    }

    // 重置光标和选择
    g_mgr->cursor_x = 0;
    g_mgr->cursor_y = 0;
    g_mgr->selected_x = -1;
    g_mgr->selected_y = -1;
    g_mgr->has_selection = false;

    g_mgr->current_game = type;
}

// 处理按键
void game_manager_handle_key(int key)
{
    ESP_LOGI("GM", "Handling key: %d, screen: %d", key, g_current_menu);

    if (g_show_splash) {
        // 任意按键退出 splash
        sound_play(SOUND_KEY_CLICK);
        g_show_splash = false;
        g_current_menu = MENU_SCREEN_MAIN;
        return;
    }

    ui_lock_acquire();

    switch (g_current_menu) {
        case MENU_SCREEN_MAIN: {
            if (key == KEY_UP) {
                if (g_mgr->menu_index > 0) {
                    g_mgr->menu_index--;
                    sound_play(SOUND_KEY_CLICK);
                }
            } else if (key == KEY_DOWN) {
                if (g_mgr->menu_index < 2) { // 3 items: Play, Settings, About
                    g_mgr->menu_index++;
                    sound_play(SOUND_KEY_CLICK);
                }
            } else if (key == KEY_A) {
                sound_play(SOUND_MENU_SELECT);
                if (g_mgr->menu_index == 0) {
                    // Play Game - 进入游戏选择
                    g_current_menu = MENU_SCREEN_GAME_SELECTOR;
                    g_mgr->menu_index = 0;
                } else if (g_mgr->menu_index == 1) {
                    // Settings - 暂时保留功能，使用 chess_ui 中的音效开关
                    chess_ui_init(); // 复用 chess_ui 的音效设置
                } else {
                    // About - 直接显示原有 chess_ui 中的 about
                    chess_ui_init();
                    chess_ui_switch_screen(SCREEN_ABOUT);
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
            }
            break;
        }

        case MENU_SCREEN_GAME_SELECTOR: {
            if (key == KEY_UP) {
                if (g_mgr->menu_index > 0) {
                    g_mgr->menu_index--;
                    sound_play(SOUND_KEY_CLICK);
                }
            } else if (key == KEY_DOWN) {
                if (g_mgr->menu_index < GAME_COUNT - 1) {
                    g_mgr->menu_index++;
                    sound_play(SOUND_KEY_CLICK);
                }
            } else if (key == KEY_A) {
                g_selected_game_idx = g_mgr->menu_index;
                sound_play(SOUND_MENU_SELECT);
                if (g_selected_game_idx == 0) {
                    // 国际象棋 - 直接调用原有 chess_ui
                    chess_ui_init();
                    chess_ui_switch_screen(SCREEN_MAIN_MENU);
                } else {
                    // 其他游戏 - 待实现
                    ESP_LOGI("GM", "Game %d not implemented yet", g_selected_game_idx);
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                g_current_menu = MENU_SCREEN_MAIN;
                g_mgr->menu_index = 0;
            }
            break;
        }

        case MENU_SCREEN_PLAYING: {
            // 游戏内控制 - 对于国际象棋，直接委托给 chess_ui
            if (g_mgr->current_game == GAME_TYPE_CHESS) {
                chess_ui_handle_key(key);
            }
            break;
        }
    }

    ui_lock_release();
}

// 渲染通用 splash screen
static void render_common_splash(void)
{
    // 画背景渐变色
    for (int y = 0; y < TFT_HEIGHT; y++) {
        uint8_t r = 0x10 + (y * 0x20 / TFT_HEIGHT);
        uint8_t g = 0x10 + (y * 0x18 / TFT_HEIGHT);
        uint8_t b = 0x08 + (y * 0x10 / TFT_HEIGHT);
        uint16_t color = RGB_TO_RGB565(r, g, b);
        for (int x = 0; x < TFT_WIDTH; x++) {
            tft_sprite_draw_pixel(&g_sprite, x, y, color);
        }
    }

    // 装饰性棋王图标
    const uint8_t king_pattern[] = {
        0x00, 0x18, 0x18, 0x3C, 0x3C, 0x7E, 0x7E, 0xDB,
        0x7E, 0x3C, 0x3C, 0x7E, 0x7E, 0x3C, 0x18, 0x00
    };
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 8; x++) {
            if (king_pattern[y] & (1 << (7 - x))) {
                tft_sprite_draw_pixel(&g_sprite, x + 4, y + 4, TFT_GOLD);
                tft_sprite_draw_pixel(&g_sprite, TFT_WIDTH - x - 5, y + 4, TFT_GOLD);
                tft_sprite_draw_pixel(&g_sprite, x + 4, TFT_HEIGHT - y - 5, TFT_GOLD);
                tft_sprite_draw_pixel(&g_sprite, TFT_WIDTH - x - 5, TFT_HEIGHT - y - 5, TFT_GOLD);
            }
        }
    }

    // 标题 "GAMES"
    const char* title = "GAMES";
    int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 3)) / 2;
    // 画轮廓
    for (int i = 0; title[i]; i++) {
        tft_sprite_draw_char(&g_sprite, title_x + i * 18 - 1, 34, title[i], TFT_BLACK, 3);
        tft_sprite_draw_char(&g_sprite, title_x + i * 18 + 1, 34, title[i], TFT_BLACK, 3);
        tft_sprite_draw_char(&g_sprite, title_x + i * 18, 33, title[i], TFT_BLACK, 3);
        tft_sprite_draw_char(&g_sprite, title_x + i * 18, 35, title[i], TFT_BLACK, 3);
    }
    // 主标题
    for (int i = 0; title[i]; i++) {
        tft_sprite_draw_char(&g_sprite, title_x + i * 18, 34, title[i], TFT_GOLD, 3);
    }

    // 副标题
    const char* subtitle = "XiaoMiao";
    int sub_x = (TFT_WIDTH - tft_sprite_text_width(subtitle, 2)) / 2;
    for (int i = 0; subtitle[i]; i++) {
        tft_sprite_draw_char(&g_sprite, sub_x + i * 12, 75, subtitle[i], TFT_YELLOW, 2);
    }

    // 提示文字
    const char* prompt = "Press any key";
    int prompt_x = (TFT_WIDTH - tft_sprite_text_width(prompt, 1)) / 2;
    for (int i = 0; prompt[i]; i++) {
        tft_sprite_draw_pixel(&g_sprite, prompt_x + i * 6, 110, TFT_LIGHTGREY);
    }
}

// 渲染主菜单
static void game_manager_render_main_menu(void)
{
    tft_sprite_fill_screen(&g_sprite, COLOR_BACKGROUND);

    // 标题
    const char* title = "XiaoMiao";
    int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 2)) / 2;
    for (int i = 0; title[i]; i++) {
        tft_sprite_draw_char(&g_sprite, title_x + i * 12, 8, title[i], TFT_GOLD, 2);
    }

    // 菜单项
    const char* menu_items[] = {"Play Games", "Settings", "About"};
    for (int i = 0; i < 3; i++) {
        int y = 35 + i * 25;
        if (i == g_mgr->menu_index) {
            tft_sprite_fill_rect(&g_sprite, 20, y, 120, 20, TFT_NAVY);
        }
        int x = 25;
        for (const char* c = menu_items[i]; *c; c++) {
            tft_sprite_draw_char(&g_sprite, x, y + 6, *c,
                (i == g_mgr->menu_index) ? TFT_YELLOW : TFT_WHITE, 1);
            x += 6;
        }
    }
}

// 渲染游戏选择菜单
static void game_manager_render_game_selector(void)
{
    tft_sprite_fill_screen(&g_sprite, COLOR_BACKGROUND);

    // 标题
    const char* title = "Select Game";
    int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 1)) / 2;
    int x = title_x;
    for (const char* c = title; *c; c++) {
        tft_sprite_draw_char(&g_sprite, x, 5, *c, TFT_WHITE, 1);
        x += 6;
    }

    // 游戏列表
    for (int i = 0; i < GAME_COUNT; i++) {
        int y = 20 + i * 25;
        if (i == g_mgr->menu_index) {
            tft_sprite_fill_rect(&g_sprite, 5, y, TFT_WIDTH - 10, 20, TFT_NAVY);
        }
        x = 10;
        const char* name = g_game_names[i];
        for (const char* c = name; *c && x < TFT_WIDTH - 10; c++) {
            tft_sprite_draw_char(&g_sprite, x, y + 6, *c,
                (i == g_mgr->menu_index) ? TFT_YELLOW : TFT_WHITE, 1);
            x += 6;
        }
    }

    // 返回提示
    const char* back = "B: Back";
    x = 10;
    for (const char* c = back; *c; c++) {
        tft_sprite_draw_char(&g_sprite, x, TFT_HEIGHT - 12, *c, TFT_LIGHTGREY, 1);
        x += 6;
    }
}

// 渲染
void game_manager_render(TFT_Sprite* sprite)
{
    int64_t now = esp_timer_get_time();
    if (now - g_last_render_tick < 33000) { // ~30 FPS
        return;
    }
    g_last_render_tick = now;

    if (!g_sprite_created) {
        return;
    }

    ui_lock_acquire();

    if (g_show_splash) {
        // 检查是否超过3秒自动退出
        if (now - g_splash_start_time > 3000000) {
            g_show_splash = false;
            g_current_menu = MENU_SCREEN_MAIN;
        } else {
            render_common_splash();
        }
    } else {
        // 根据当前菜单渲染
        switch (g_current_menu) {
            case MENU_SCREEN_SPLASH:
                render_common_splash();
                break;
            case MENU_SCREEN_MAIN:
                game_manager_render_main_menu();
                break;
            case MENU_SCREEN_GAME_SELECTOR:
                game_manager_render_game_selector();
                break;
            case MENU_SCREEN_PLAYING:
                // 对于国际象棋，直接使用 chess_ui 渲染
                chess_ui_render_locked();
                ui_lock_release();
                return;
        }
    }

    // 推送到屏幕
    tft_sprite_push(&g_sprite, 0, 0);

    ui_lock_release();
}

// 更新
void game_manager_update(void)
{
    // 对于国际象棋，调用 chess_ui_update
    if (g_mgr->current_game == GAME_TYPE_CHESS) {
        chess_ui_update();
    }
}

// 获取游戏接口列表
const GameInterface** game_manager_get_game_list(int* count)
{
    if (count != NULL) {
        *count = GAME_COUNT;
    }
    return g_game_interfaces;
}

// ==================== 国际象棋适配层实现 ====================

// 适配层使用的本地状态
static ChessGame* g_adapter_game = NULL;
static ChessPiece g_selected_piece = {PIECE_NONE, COLOR_WHITE, false};
static int g_selected_x = -1, g_selected_y = -1;

static void chess_adapter_init(void* game)
{
    // 这里我们不直接初始化，而是让 chess_ui 来管理
    g_adapter_game = (ChessGame*)game;
    chess_game_init(g_adapter_game);
}

static void chess_adapter_reset(void* game)
{
    chess_game_reset((ChessGame*)game);
}

static bool chess_adapter_select_piece(void* game, int x, int y)
{
    ChessPiece piece = chess_game_get_piece((ChessGame*)game, x, y);
    if (piece.type != PIECE_NONE) {
        g_selected_piece = piece;
        g_selected_x = x;
        g_selected_y = y;
        return true;
    }
    return false;
}

static bool chess_adapter_make_move(void* game, int x1, int y1, int x2, int y2)
{
    // 国际象棋没有升变选择，传 PIECE_QUEEN
    return chess_game_make_move((ChessGame*)game, x1, y1, x2, y2, PIECE_QUEEN);
}

static bool chess_adapter_is_valid_move(void* game, int x1, int y1, int x2, int y2)
{
    return chess_game_is_valid_move((ChessGame*)game, x1, y1, x2, y2);
}

static int chess_adapter_get_current_player(void* game)
{
    ChessColor color = chess_game_get_current_turn((ChessGame*)game);
    return (color == COLOR_WHITE) ? 1 : 2;
}

static bool chess_adapter_is_game_over(void* game)
{
    return chess_game_is_game_over((ChessGame*)game);
}

static GameResult chess_adapter_get_result(void* game)
{
    ChessColor winner = chess_game_get_winner((ChessGame*)game);
    if (winner == COLOR_WHITE) return GAME_RESULT_WHITE_WIN;
    if (winner == COLOR_BLACK) return GAME_RESULT_BLACK_WIN;
    return GAME_RESULT_DRAW;
}

static int chess_adapter_board_width(void)
{
    return CHESS_BOARD_SIZE;
}

static int chess_adapter_board_height(void)
{
    return CHESS_BOARD_SIZE;
}

static void chess_adapter_render(void* game, TFT_Sprite* sprite, int offset_x, int offset_y,
                                 int cursor_x, int cursor_y, int sel_x, int sel_y)
{
    // 实际的棋盘渲染在 chess_ui.c 中，这里不重复实现
    (void)game; (void)sprite; (void)offset_x; (void)offset_y;
    (void)cursor_x; (void)cursor_y; (void)sel_x; (void)sel_y;
}
