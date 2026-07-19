#include "game_menu.h"
#include "sound.h"
#include "tft_display.h"
#include "ui_lock.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "GameMenu";

// 菜单状态
typedef struct {
    GameMenuState state;
    GameMenuState prev_state;
    int menu_index;
    int menu_count;

    const GameMenuConfig* config;
    const GameMenuCallbacks* callbacks;

    // 消息显示
    bool show_message;
    char message_text[64];
    int message_timer;

    // 渲染节流
    int64_t last_render_time;

    // 启动画面计时器
    int64_t splash_start_time;

    // 精灵缓存
    TFT_Sprite sprite;
    bool sprite_created;
} GameMenuContext;

static GameMenuContext* g_ctx = NULL;

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

void game_menu_init(const GameMenuConfig* config, const GameMenuCallbacks* callbacks)
{
    if (g_ctx != NULL) {
        game_menu_deinit();
    }

    ESP_LOGI(TAG, "Initializing game menu for: %s", config->game_name);

    g_ctx = (GameMenuContext*)calloc(1, sizeof(GameMenuContext));
    if (!g_ctx) {
        ESP_LOGE(TAG, "Failed to allocate menu context");
        return;
    }

    g_ctx->config = config;
    g_ctx->callbacks = callbacks;
    g_ctx->state = GAME_MENU_STATE_SPLASH;
    g_ctx->prev_state = GAME_MENU_STATE_SPLASH;
    g_ctx->menu_index = 0;
    g_ctx->menu_count = 0;
    g_ctx->show_message = false;
    g_ctx->last_render_time = 0;
    g_ctx->splash_start_time = esp_timer_get_time();

    if (tft_sprite_create(&g_ctx->sprite, TFT_WIDTH, TFT_HEIGHT) == ESP_OK) {
        g_ctx->sprite_created = true;
    }
}

void game_menu_deinit(void)
{
    if (g_ctx != NULL) {
        if (g_ctx->sprite_created) {
            tft_sprite_delete(&g_ctx->sprite);
            g_ctx->sprite_created = false;
        }
        free(g_ctx);
        g_ctx = NULL;
    }
}

void game_menu_switch_state(GameMenuState state)
{
    if (!g_ctx) return;

    ESP_LOGI(TAG, "Switching menu state to: %d", state);

    g_ctx->prev_state = g_ctx->state;
    g_ctx->state = state;
    g_ctx->menu_index = 0;

    switch (state) {
        case GAME_MENU_STATE_SPLASH:
            g_ctx->menu_count = 0;
            g_ctx->splash_start_time = esp_timer_get_time();
            break;
        case GAME_MENU_STATE_MAIN:
            g_ctx->menu_count = 3;
            break;
        case GAME_MENU_STATE_BLUETOOTH:
            g_ctx->menu_count = 3;
            break;
        case GAME_MENU_STATE_GAME:
            g_ctx->menu_count = 0;
            break;
        case GAME_MENU_STATE_EXIT_CONFIRM:
            g_ctx->menu_count = 2;
            break;
    }
}

GameMenuState game_menu_get_state(void)
{
    return g_ctx ? g_ctx->state : GAME_MENU_STATE_MAIN;
}

void game_menu_show_message(const char* text, int duration_ms)
{
    if (!g_ctx || !text) return;

    strncpy(g_ctx->message_text, text, sizeof(g_ctx->message_text) - 1);
    g_ctx->message_text[sizeof(g_ctx->message_text) - 1] = '\0';
    g_ctx->show_message = true;
    g_ctx->message_timer = duration_ms / 100;
}

void game_menu_update(void)
{
    if (!g_ctx) return;

    // 启动画面自动跳转
    if (g_ctx->state == GAME_MENU_STATE_SPLASH) {
        int64_t now = esp_timer_get_time();
        if (now - g_ctx->splash_start_time > 3000000) {
            game_menu_switch_state(GAME_MENU_STATE_MAIN);
        }
    }

    // 更新消息计时器
    if (g_ctx->show_message) {
        if (g_ctx->message_timer > 0) {
            g_ctx->message_timer--;
        } else {
            g_ctx->show_message = false;
        }
    }
}

static void render_splash(void)
{
    // 绘制渐变背景
    for (int y = 0; y < TFT_HEIGHT; y++) {
        uint8_t r = 0x10 + (y * 0x20 / TFT_HEIGHT);
        uint8_t g = 0x10 + (y * 0x18 / TFT_HEIGHT);
        uint8_t b = 0x08 + (y * 0x10 / TFT_HEIGHT);
        uint16_t color = RGB_TO_RGB565(r, g, b);
        for (int x = 0; x < TFT_WIDTH; x++) {
            tft_sprite_draw_pixel(&g_ctx->sprite, x, y, color);
        }
    }

    // 绘制装饰性图案
    const uint8_t king_pattern[] = {
        0x00, 0x18, 0x18, 0x3C, 0x3C, 0x7E, 0x7E, 0xDB,
        0x7E, 0x3C, 0x3C, 0x7E, 0x7E, 0x3C, 0x18, 0x00
    };
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 8; x++) {
            if (king_pattern[y] & (1 << (7 - x))) {
                tft_sprite_draw_pixel(&g_ctx->sprite, x + 4, y + 4, TFT_GOLD);
                tft_sprite_draw_pixel(&g_ctx->sprite, TFT_WIDTH - x - 5, y + 4, TFT_GOLD);
                tft_sprite_draw_pixel(&g_ctx->sprite, x + 4, TFT_HEIGHT - y - 5, TFT_GOLD);
                tft_sprite_draw_pixel(&g_ctx->sprite, TFT_WIDTH - x - 5, TFT_HEIGHT - y - 5, TFT_GOLD);
            }
        }
    }

    // 绘制标题
    const char* title = g_ctx->config->splash_title;
    int title_width = tft_sprite_text_width(title, 3);
    int title_x = (TFT_WIDTH - title_width) / 2;
    int title_y = 35;

    // 绘制轮廓
    for (int i = 0, x = title_x; title[i]; i++) {
        tft_sprite_draw_char(&g_ctx->sprite, x - 1, title_y - 1, title[i], TFT_BLACK, 3);
        tft_sprite_draw_char(&g_ctx->sprite, x + 1, title_y - 1, title[i], TFT_BLACK, 3);
        tft_sprite_draw_char(&g_ctx->sprite, x - 1, title_y + 1, title[i], TFT_BLACK, 3);
        tft_sprite_draw_char(&g_ctx->sprite, x + 1, title_y + 1, title[i], TFT_BLACK, 3);
        x += 18;
    }

    // 绘制主标题
    for (int i = 0, x = title_x; title[i]; i++) {
        tft_sprite_draw_char(&g_ctx->sprite, x, title_y, title[i], TFT_GOLD, 3);
        x += 18;
    }

    // 绘制副标题
    const char* subtitle = g_ctx->config->splash_subtitle;
    if (subtitle && subtitle[0]) {
        int subtitle_x = (TFT_WIDTH - tft_sprite_text_width(subtitle, 2)) / 2;
        int text_x = subtitle_x;
        for (const char* p = subtitle; *p; p++) {
            tft_sprite_draw_char(&g_ctx->sprite, text_x, 75, *p, TFT_YELLOW, 2);
            text_x += 12;
        }
    }

    // 绘制按键提示
    const char* hint = "Press any key";
    int hint_x = (TFT_WIDTH - tft_sprite_text_width(hint, 1)) / 2;
    int text_x = hint_x;
    for (const char* p = hint; *p; p++) {
        tft_sprite_draw_pixel(&g_ctx->sprite, text_x, 110, *p, TFT_LIGHTGREY, 1);
        text_x += 6;
    }
}

static void render_main_menu(void)
{
    // 标题
    const char* title = g_ctx->config->game_name;
    int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 2)) / 2;
    for (int i = 0, x = title_x; title[i]; i++) {
        tft_sprite_draw_char(&g_ctx->sprite, x, 8, title[i], TFT_WHITE, 2);
        x += 12;
    }

    // 菜单项
    for (int i = 0; i < 3; i++) {
        int y = 35 + i * 25;
        bool is_selected = (i == g_ctx->menu_index);

        if (is_selected) {
            tft_sprite_fill_rect(&g_ctx->sprite, 20, y, 120, 20, TFT_NAVY);
        }

        int text_x = 25;
        const char* text = main_menu_items[i];
        while (*text) {
            tft_sprite_draw_char(&g_ctx->sprite, text_x, y + 6, *text, is_selected ? TFT_YELLOW : TFT_WHITE, 1);
            text_x += 6;
            text++;
        }
    }
}

static void render_bluetooth_menu(void)
{
    // 标题
    const char* title = "Bluetooth Game";
    int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 1)) / 2;
    int text_x = title_x;
    for (const char* p = title; *p; p++) {
        tft_sprite_draw_char(&g_ctx->sprite, text_x, 5, *p, TFT_WHITE, 1);
        text_x += 6;
    }

    // 菜单项
    for (int i = 0; i < 3; i++) {
        int y = 35 + i * 25;
        bool is_selected = (i == g_ctx->menu_index);

        if (is_selected) {
            tft_sprite_fill_rect(&g_ctx->sprite, 20, y, 120, 20, TFT_NAVY);
        }

        text_x = 25;
        const char* text = bluetooth_menu_items[i];
        while (*text) {
            tft_sprite_draw_char(&g_ctx->sprite, text_x, y + 6, *text, is_selected ? TFT_YELLOW : TFT_WHITE, 1);
            text_x += 6;
            text++;
        }
    }
}

static void render_exit_confirm(void)
{
    // 绘制对话框背景
    tft_sprite_fill_rect(&g_ctx->sprite, 20, 30, 120, 70, COLOR_DIALOG);
    tft_sprite_draw_rect(&g_ctx->sprite, 20, 30, 120, 70, TFT_WHITE);

    // 标题
    const char* title = "Exit Game?";
    int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 1)) / 2;
    int text_x = title_x;
    for (const char* p = title; *p; p++) {
        tft_sprite_draw_char(&g_ctx->sprite, text_x, 45, *p, TFT_WHITE, 1);
        text_x += 6;
    }

    // 菜单项
    for (int i = 0; i < 2; i++) {
        int y = 65 + i * 20;
        bool is_selected = (i == g_ctx->menu_index);

        if (is_selected) {
            tft_sprite_fill_rect(&g_ctx->sprite, 35, y, 90, 18, TFT_NAVY);
        }

        text_x = 40;
        const char* text = exit_confirm_items[i];
        while (*text) {
            tft_sprite_draw_char(&g_ctx->sprite, text_x, y + 5, *text, is_selected ? TFT_YELLOW : TFT_WHITE, 1);
            text_x += 6;
            text++;
        }
    }
}

static void render_message(void)
{
    // 简单消息框
    int msg_width = tft_sprite_text_width(g_ctx->message_text, 1) + 10;
    int msg_x = (TFT_WIDTH - msg_width) / 2;
    int msg_y = 50;

    tft_sprite_fill_rect(&g_ctx->sprite, msg_x - 5, msg_y - 5, msg_width + 10, 22, COLOR_DIALOG);
    tft_sprite_draw_rect(&g_ctx->sprite, msg_x - 5, msg_y - 5, msg_width + 10, 22, TFT_WHITE);

    int text_x = msg_x;
    const char* text = g_ctx->message_text;
    while (*text) {
        tft_sprite_draw_char(&g_ctx->sprite, text_x, msg_y, *text, TFT_WHITE, 1);
        text_x += 6;
        text++;
    }
}

static void render_internal(bool force)
{
    if (!g_ctx || !g_ctx->sprite_created) return;

    // 渲染节流
    int64_t now = esp_timer_get_time();
    if (!force && now - g_ctx->last_render_time < 33000) {
        return;
    }
    g_ctx->last_render_time = now;

    // 清空屏幕
    tft_sprite_fill_screen(&g_ctx->sprite, COLOR_BACKGROUND);

    // 渲染当前状态
    switch (g_ctx->state) {
        case GAME_MENU_STATE_SPLASH:
            render_splash();
            break;
        case GAME_MENU_STATE_MAIN:
            render_main_menu();
            break;
        case GAME_MENU_STATE_BLUETOOTH:
            render_bluetooth_menu();
            break;
        case GAME_MENU_STATE_GAME:
            if (g_ctx->callbacks && g_ctx->callbacks->render_custom) {
                g_ctx->callbacks->render_custom();
            }
            break;
        case GAME_MENU_STATE_EXIT_CONFIRM:
            // 需要先渲染游戏背景，所以调用自定义渲染
            if (g_ctx->callbacks && g_ctx->callbacks->render_custom) {
                g_ctx->callbacks->render_custom();
            }
            render_exit_confirm();
            break;
    }

    // 渲染消息（如果有）
    if (g_ctx->show_message && g_ctx->state != GAME_MENU_STATE_EXIT_CONFIRM) {
        render_message();
    }

    // 推送屏幕
    tft_sprite_push(&g_ctx->sprite, 0, 0);
}

void game_menu_render(void)
{
    ui_lock_acquire();
    render_internal(false);
    ui_lock_release();
}

void game_menu_render_force(void)
{
    ui_lock_acquire();
    render_internal(true);
    ui_lock_release();
}

void game_menu_handle_key(KeyCode key)
{
    if (!g_ctx) return;

    ESP_LOGI(TAG, "Handling key %d in state %d", key, g_ctx->state);

    ui_lock_acquire();

    switch (g_ctx->state) {
        case GAME_MENU_STATE_SPLASH:
            // 任意键进入主菜单
            sound_play(SOUND_KEY_CLICK);
            game_menu_switch_state(GAME_MENU_STATE_MAIN);
            break;

        case GAME_MENU_STATE_MAIN:
            if (key == KEY_UP) {
                if (g_ctx->menu_index > 0) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ctx->menu_index--;
                }
            } else if (key == KEY_DOWN) {
                if (g_ctx->menu_index < g_ctx->menu_count - 1) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ctx->menu_index++;
                }
            } else if (key == KEY_A) {
                sound_play(SOUND_MENU_SELECT);
                if (g_ctx->menu_index == 0) {
                    // 本地游戏
                    if (g_ctx->callbacks && g_ctx->callbacks->on_local_game) {
                        g_ctx->callbacks->on_local_game();
                    }
                } else if (g_ctx->menu_index == 1) {
                    // 蓝牙游戏
                    game_menu_switch_state(GAME_MENU_STATE_BLUETOOTH);
                } else if (g_ctx->menu_index == 2) {
                    // 返回游戏选择器
                    if (g_ctx->callbacks && g_ctx->callbacks->on_back_to_selector) {
                        g_ctx->callbacks->on_back_to_selector();
                    }
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                // 返回游戏选择器
                if (g_ctx->callbacks && g_ctx->callbacks->on_back_to_selector) {
                    g_ctx->callbacks->on_back_to_selector();
                }
            }
            break;

        case GAME_MENU_STATE_BLUETOOTH:
            if (key == KEY_UP) {
                if (g_ctx->menu_index > 0) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ctx->menu_index--;
                }
            } else if (key == KEY_DOWN) {
                if (g_ctx->menu_index < g_ctx->menu_count - 1) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ctx->menu_index++;
                }
            } else if (key == KEY_A) {
                sound_play(SOUND_MENU_SELECT);
                if (g_ctx->menu_index == 0) {
                    // 创建游戏
                    if (g_ctx->callbacks && g_ctx->callbacks->on_create_game) {
                        g_ctx->callbacks->on_create_game();
                    }
                } else if (g_ctx->menu_index == 1) {
                    // 加入游戏
                    if (g_ctx->callbacks && g_ctx->callbacks->on_join_game) {
                        g_ctx->callbacks->on_join_game();
                    }
                } else if (g_ctx->menu_index == 2) {
                    // 返回主菜单
                    game_menu_switch_state(GAME_MENU_STATE_MAIN);
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                game_menu_switch_state(GAME_MENU_STATE_MAIN);
            }
            break;

        case GAME_MENU_STATE_EXIT_CONFIRM:
            if (key == KEY_UP) {
                if (g_ctx->menu_index > 0) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ctx->menu_index--;
                }
            } else if (key == KEY_DOWN) {
                if (g_ctx->menu_index < g_ctx->menu_count - 1) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ctx->menu_index++;
                }
            } else if (key == KEY_A) {
                sound_play(SOUND_MENU_SELECT);
                if (g_ctx->menu_index == 0) {
                    // 确认退出
                    game_menu_switch_state(GAME_MENU_STATE_MAIN);
                } else {
                    // 取消，返回游戏
                    game_menu_switch_state(g_ctx->prev_state);
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                // 取消，返回游戏
                game_menu_switch_state(g_ctx->prev_state);
            }
            break;

        case GAME_MENU_STATE_GAME:
            // 游戏中的按键由游戏自己处理
            break;
    }

    ui_lock_release();
}
