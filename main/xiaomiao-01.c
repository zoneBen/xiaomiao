#include <stdio.h>
#include <unistd.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "chess.h"
#include "chess_ui.h"
#include "chess_bluetooth.h"
#include "ui_lock.h"
#include "tft_display.h"
#include "sound.h"
#include "xiangqi.h"
#include "xiangqi_ui.h"
#include "gomoku.h"
#include "gomoku_ui.h"
#include "go.h"
#include "go_ui.h"
#include "junqi.h"
#include "junqi_ui.h"

static const char* TAG = "xiaomiao";

// 游戏选择
typedef enum {
    GAME_SELECTOR,
    GAME_CHESS,
    GAME_XIANGQI,
    GAME_GOMOKU,
    GAME_GO,
    GAME_JUNQI,
} CurrentGame;

// 顶级菜单状态
typedef enum {
    TOP_MENU_GAMES,
    TOP_MENU_SOUND,
    TOP_MENU_ABOUT,
} TopMenuState;

static CurrentGame s_current_game = GAME_SELECTOR;
static int s_game_selector_index = 0;
static TopMenuState s_top_menu_state = TOP_MENU_GAMES;

// Flags to request UI updates from Bluetooth callbacks
static volatile bool s_render_requested = false;
static volatile bool s_screen_switch_requested = false;
static volatile ChessScreen s_requested_screen = SCREEN_MAIN_MENU;

// Structure to hold pending move from opponent
static volatile bool s_move_pending = false;
static volatile int s_move_from_x = 0;
static volatile int s_move_from_y = 0;
static volatile int s_move_to_x = 0;
static volatile int s_move_to_y = 0;
static volatile ChessPieceType s_move_promotion = PIECE_NONE;

// Structure to hold pending game sync
static volatile bool s_sync_pending = false;
static ChessGame s_synced_game;

static TFT_Sprite s_sprite;
static bool s_sprite_created = false;

// 国际象棋UI退出标志
static volatile bool chess_ui_should_exit_flag = false;

// 设置国际象棋退出标志
void chess_ui_set_should_exit(void);

static void render_top_menu(void)
{
    // 画背景
    tft_sprite_fill_screen(&s_sprite, COLOR_BACKGROUND);

    // 顶部标签栏
    const char* tabs[] = {"Games", "Sound", "About"};
    for (int i = 0; i < 3; i++) {
        int x = 10 + i * 50;
        bool selected = (i == s_top_menu_state);
        if (selected) {
            tft_sprite_fill_rect(&s_sprite, x, 2, 45, 12, TFT_NAVY);
        }
        int tx = x + 5;
        for (const char* p = tabs[i]; *p; p++) {
            tft_sprite_draw_char(&s_sprite, tx, 4, *p, selected ? TFT_YELLOW : TFT_LIGHTGREY, 1);
            tx += 6;
        }
    }

    if (s_top_menu_state == TOP_MENU_GAMES) {
        // 游戏选择
        const char* title = "SELECT GAME";
        int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 1)) / 2;
        int x = title_x;
        for (const char* p = title; *p; p++) {
            tft_sprite_draw_char(&s_sprite, x, 20, *p, TFT_YELLOW, 1);
            x += 6;
        }

        const char* games[] = {"1. Chess", "2. XiangQi", "3. Gomoku", "4. Go", "5. JunQi"};
        for (int i = 0; i < 5; i++) {
            int y = 35 + i * 16;
            bool selected = (i == s_game_selector_index);
            if (selected) {
                tft_sprite_fill_rect(&s_sprite, 15, y, TFT_WIDTH - 30, 14, TFT_NAVY);
            }
            x = 20;
            for (const char* p = games[i]; *p; p++) {
                tft_sprite_draw_char(&s_sprite, x, y + 3, *p, selected ? TFT_YELLOW : TFT_WHITE, 1);
                x += 6;
            }
        }
    } else if (s_top_menu_state == TOP_MENU_SOUND) {
        // 声音设置
        const char* title = "SOUND SETTINGS";
        int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 1)) / 2;
        int x = title_x;
        for (const char* p = title; *p; p++) {
            tft_sprite_draw_char(&s_sprite, x, 20, *p, TFT_YELLOW, 1);
            x += 6;
        }

        const char* sound_items[] = {"Sound: On", "Sound: Off"};
        bool sound_on = sound_get_effects_enabled();
        for (int i = 0; i < 2; i++) {
            int y = 40 + i * 20;
            bool selected = ((sound_on && i == 0) || (!sound_on && i == 1));
            if (selected) {
                tft_sprite_fill_rect(&s_sprite, 15, y, TFT_WIDTH - 30, 16, TFT_NAVY);
            }
            x = 20;
            for (const char* p = sound_items[i]; *p; p++) {
                tft_sprite_draw_char(&s_sprite, x, y + 4, *p, selected ? TFT_YELLOW : TFT_WHITE, 1);
                x += 6;
            }
        }
    } else if (s_top_menu_state == TOP_MENU_ABOUT) {
        // 关于页面
        const char* title = "ABOUT";
        int title_x = (TFT_WIDTH - tft_sprite_text_width(title, 2)) / 2;
        int text_x = title_x;
        for (int i = 0; title[i]; i++) {
            tft_sprite_draw_char(&s_sprite, text_x, 15, title[i], TFT_GOLD, 2);
            text_x += 12;
        }

        // 装饰性棋子图案
        const uint8_t king_pattern[] = {
            0x00, 0x18, 0x18, 0x3C, 0x3C, 0x7E, 0x7E, 0xDB,
            0x7E, 0x3C, 0x3C, 0x7E, 0x7E, 0x3C, 0x18, 0x00
        };
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 8; x++) {
                if (king_pattern[y] & (1 << (7 - x))) {
                    tft_sprite_draw_pixel(&s_sprite, x + 8, y + 35, TFT_GOLD);
                    tft_sprite_draw_pixel(&s_sprite, TFT_WIDTH - x - 9, y + 35, TFT_GOLD);
                }
            }
        }

        // 开发者信息
        const char* dev_label = "By: ";
        const char* dev_name = "bilibili Zoneben";
        int label_width = tft_sprite_text_width(dev_label, 1);
        int name_width = tft_sprite_text_width(dev_name, 1);
        int total_width = label_width + name_width;
        int start_x = (TFT_WIDTH - total_width) / 2;

        text_x = start_x;
        for (const char* p = dev_label; *p; p++) {
            tft_sprite_draw_char(&s_sprite, text_x, 55, *p, TFT_LIGHTGREY, 1);
            text_x += 6;
        }

        int name_x = start_x + label_width;
        tft_sprite_fill_rect(&s_sprite, name_x - 2, 53, name_width + 4, 12, TFT_NAVY);
        text_x = name_x;
        for (const char* p = dev_name; *p; p++) {
            tft_sprite_draw_char(&s_sprite, text_x, 55, *p, TFT_YELLOW, 1);
            text_x += 6;
        }

        // 分割线
        tft_sprite_fill_rect(&s_sprite, 20, 72, TFT_WIDTH - 40, 1, TFT_LIGHTGREY);

        // 联系方式
        const char* qq_label = "QQ: ";
        const char* qq_num = "809468582";
        int qq_label_width = tft_sprite_text_width(qq_label, 1);
        int qq_num_width = tft_sprite_text_width(qq_num, 1);
        int qq_total_width = qq_label_width + qq_num_width;
        int qq_start_x = (TFT_WIDTH - qq_total_width) / 2;

        text_x = qq_start_x;
        for (const char* p = qq_label; *p; p++) {
            tft_sprite_draw_char(&s_sprite, text_x, 80, *p, TFT_LIGHTGREY, 1);
            text_x += 6;
        }

        int qq_num_x = qq_start_x + qq_label_width;
        tft_sprite_fill_rect(&s_sprite, qq_num_x - 2, 78, qq_num_width + 4, 12, TFT_NAVY);
        text_x = qq_num_x;
        for (const char* p = qq_num; *p; p++) {
            tft_sprite_draw_char(&s_sprite, text_x, 80, *p, TFT_YELLOW, 1);
            text_x += 6;
        }
    }
}


#define KEY_UP_GPIO 2
#define KEY_DOWN_GPIO 13
#define KEY_LEFT_GPIO 27
#define KEY_RIGHT_GPIO 35
#define KEY_A_GPIO 34
#define KEY_B_GPIO 12

typedef struct {
    int gpio;
    KeyCode key;
    const char* name;
    bool last_state;
    bool pressed;
    TickType_t debounce_tick;
} KeyState;

static KeyState keys[] = {
    {KEY_UP_GPIO, KEY_UP, "UP", false, false, 0},
    {KEY_DOWN_GPIO, KEY_DOWN, "DOWN", false, false, 0},
    {KEY_LEFT_GPIO, KEY_LEFT, "LEFT", false, false, 0},
    {KEY_RIGHT_GPIO, KEY_RIGHT, "RIGHT", false, false, 0},
    {KEY_A_GPIO, KEY_A, "A", false, false, 0},
    {KEY_B_GPIO, KEY_B, "B", false, false, 0},
};


static void key_init(void)
{
    // Configure GPIOs with pull-up (2, 12, 13, 27)
    gpio_config_t io_conf = {0};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << KEY_UP_GPIO) |
                           (1ULL << KEY_DOWN_GPIO) |
                           (1ULL << KEY_LEFT_GPIO) |
                           (1ULL << KEY_B_GPIO);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // Configure input-only GPIOs - note: these don't support internal pull-up
    // They rely on external pull-up resistors on the board
    gpio_config_t io_conf_input_only = {0};
    io_conf_input_only.intr_type = GPIO_INTR_DISABLE;
    io_conf_input_only.mode = GPIO_MODE_INPUT;
    io_conf_input_only.pin_bit_mask = (1ULL << KEY_RIGHT_GPIO) |
                                      (1ULL << KEY_A_GPIO);
    io_conf_input_only.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf_input_only.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf_input_only);

    ESP_LOGI(TAG, "GPIO initialized: all buttons active-low");
}

static void key_scan_task(void* arg)
{
    TickType_t last_wakeup = xTaskGetTickCount();
    const TickType_t scan_delay = pdMS_TO_TICKS(10);
    const TickType_t min_delay = (scan_delay > 0) ? scan_delay : 1;
    static int log_counter = 0;
    static bool first_log = true;

    ESP_LOGI(TAG, "Key scan task started");

    while (1) {
        for (int i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            bool raw = gpio_get_level(keys[i].gpio);
            bool current = (raw == 0);  // Low = pressed for pull-up configuration
            TickType_t now = xTaskGetTickCount();

            if (current != keys[i].last_state) {
                keys[i].debounce_tick = now;
            }

            if ((now - keys[i].debounce_tick) >= pdMS_TO_TICKS(25)) {
                if (current != keys[i].pressed) {
                    keys[i].pressed = current;
                    if (current) {
                        ESP_LOGI(TAG, ">>> Key %s (GPIO%d) PRESSED! raw=%d",
                                 keys[i].name, keys[i].gpio, raw);
                        sound_play(SOUND_KEY_CLICK);

                        if (s_current_game == GAME_SELECTOR) {
                            // 顶级菜单
                            if (keys[i].key == KEY_LEFT) {
                                if (s_top_menu_state > 0) s_top_menu_state--;
                            } else if (keys[i].key == KEY_RIGHT) {
                                if (s_top_menu_state < 2) s_top_menu_state++;
                            } else if (keys[i].key == KEY_UP) {
                                if (s_top_menu_state == TOP_MENU_GAMES) {
                                    if (s_game_selector_index > 0) s_game_selector_index--;
                                }
                            } else if (keys[i].key == KEY_DOWN) {
                                if (s_top_menu_state == TOP_MENU_GAMES) {
                                    if (s_game_selector_index < 4) s_game_selector_index++;
                                }
                            } else if (keys[i].key == KEY_A) {
                                if (s_top_menu_state == TOP_MENU_GAMES) {
                                    if (s_game_selector_index == 0) {
                                        // 国际象棋
                                        s_current_game = GAME_CHESS;
                                        ui_lock_acquire();
                                        chess_ui_init();
                                        chess_ui_switch_screen(SCREEN_MAIN_MENU);
                                        ui_lock_release();
                                    } else if (s_game_selector_index == 1) {
                                        // 中国象棋
                                        s_current_game = GAME_XIANGQI;
                                        ui_lock_acquire();
                                        xiangqi_ui_init();
                                        xiangqi_ui_switch_to_game();
                                        ui_lock_release();
                                    } else if (s_game_selector_index == 2) {
                                        // 五子棋
                                        s_current_game = GAME_GOMOKU;
                                        ui_lock_acquire();
                                        gomoku_ui_init();
                                        gomoku_ui_switch_to_game();
                                        ui_lock_release();
                                    } else if (s_game_selector_index == 3) {
                                        // 围棋
                                        s_current_game = GAME_GO;
                                        ui_lock_acquire();
                                        go_ui_init();
                                        go_ui_switch_to_game();
                                        ui_lock_release();
                                    } else if (s_game_selector_index == 4) {
                                        // 军棋
                                        s_current_game = GAME_JUNQI;
                                        ui_lock_acquire();
                                        junqi_ui_init();
                                        junqi_ui_switch_to_game();
                                        ui_lock_release();
                                    }
                                } else if (s_top_menu_state == TOP_MENU_SOUND) {
                                    // 切换声音
                                    bool current = sound_get_effects_enabled();
                                    sound_set_effects_enabled(!current);
                                }
                            }
                            s_render_requested = true;
                        } else if (s_current_game == GAME_CHESS) {
                            chess_ui_handle_key(keys[i].key);
                        } else if (s_current_game == GAME_XIANGQI) {
                            xiangqi_ui_handle_key(keys[i].key);
                        } else if (s_current_game == GAME_GOMOKU) {
                            gomoku_ui_handle_key(keys[i].key);
                        } else if (s_current_game == GAME_GO) {
                            go_ui_handle_key(keys[i].key);
                        } else if (s_current_game == GAME_JUNQI) {
                            junqi_ui_handle_key(keys[i].key);
                        }
                    } else {
                        ESP_LOGI(TAG, "<<< Key %s (GPIO%d) released",
                                 keys[i].name, keys[i].gpio);
                    }
                }
            }

            keys[i].last_state = current;
        }

        // Log raw state every ~1 second
        log_counter++;
        if (log_counter >= 100 || first_log) {
            first_log = false;
            log_counter = 0;
            ESP_LOGI(TAG, "RAW: UP=%d DOWN=%d LEFT=%d RIGHT=%d A=%d B=%d",
                     gpio_get_level(KEY_UP_GPIO), gpio_get_level(KEY_DOWN_GPIO),
                     gpio_get_level(KEY_LEFT_GPIO), gpio_get_level(KEY_RIGHT_GPIO),
                     gpio_get_level(KEY_A_GPIO), gpio_get_level(KEY_B_GPIO));
        }

        vTaskDelayUntil(&last_wakeup, min_delay);
    }
}

static void ui_update_task(void* arg)
{
    TickType_t last_wakeup = xTaskGetTickCount();
    const TickType_t update_delay = pdMS_TO_TICKS(33);  // 30 FPS

    ESP_LOGI(TAG, "UI update task started");

    // Local copies for thread-safe operation
    bool need_screen_switch = false;
    bool need_apply_move = false;
    bool need_apply_sync = false;
    bool need_render = false;
    ChessScreen requested_screen = SCREEN_MAIN_MENU;
    int move_from_x = 0, move_from_y = 0, move_to_x = 0, move_to_y = 0;
    ChessPieceType move_promotion = PIECE_NONE;
    ChessGame synced_game;

    while (1) {
        // ESP_LOGI(TAG, "UI update task loop running");

        // Step 1: Check and collect requests without holding the lock for too long
        ui_lock_acquire();
        // ESP_LOGI(TAG, "UI update task: collected requests");

        need_screen_switch = s_screen_switch_requested;
        if (need_screen_switch) {
            s_screen_switch_requested = false;
            requested_screen = s_requested_screen;
        }

        need_apply_move = s_move_pending;
        if (need_apply_move) {
            s_move_pending = false;
            move_from_x = s_move_from_x;
            move_from_y = s_move_from_y;
            move_to_x = s_move_to_x;
            move_to_y = s_move_to_y;
            move_promotion = s_move_promotion;
        }

        need_apply_sync = s_sync_pending;
        if (need_apply_sync) {
            s_sync_pending = false;
            memcpy(&synced_game, &s_synced_game, sizeof(ChessGame));
        }

        need_render = s_render_requested;
        if (need_render) {
            s_render_requested = false;
        }

        ui_lock_release();
        // ESP_LOGI(TAG, "UI update task: released lock 1");

        // Step 2: Process requests - hold the lock while modifying UI state
        ui_lock_acquire();
        // ESP_LOGI(TAG, "UI update task: processing requests");

        if (s_current_game == GAME_SELECTOR) {
            // 顶级菜单
            render_top_menu();
            tft_sprite_push(&s_sprite, 0, 0);
        } else if (s_current_game == GAME_CHESS) {
            // 国际象棋
            if (chess_ui_should_exit_flag) {
                chess_ui_should_exit_flag = false;
                chess_ui_deinit();
                s_current_game = GAME_SELECTOR;
                s_game_selector_index = 0;
                s_render_requested = true;
            } else {
                if (need_screen_switch) {
                    ESP_LOGI(TAG, "Switching screen to %d from UI task", requested_screen);
                    chess_ui_switch_screen(requested_screen);
                }

                if (need_apply_move) {
                    ESP_LOGI(TAG, "Applying opponent move from UI task");
                    chess_ui_apply_opponent_move_locked(move_from_x, move_from_y, move_to_x, move_to_y, move_promotion);
                }

                if (need_apply_sync) {
                    ESP_LOGI(TAG, "Applying game sync from UI task (locked version)");
                    chess_ui_update_game_locked(&synced_game);
                }

                if (need_screen_switch) {
                    chess_ui_render_locked_force();
                } else if (need_render) {
                    chess_ui_render_locked();
                }

                chess_ui_update();
            }
        } else if (s_current_game == GAME_XIANGQI) {
            // 中国象棋
            if (xiangqi_ui_should_exit()) {
                // 返回游戏选择菜单
                xiangqi_ui_deinit();
                s_current_game = GAME_SELECTOR;
                s_game_selector_index = 0;
                s_render_requested = true;
            } else {
                xiangqi_ui_update();
                if (need_render) {
                    xiangqi_ui_render_locked();  // 使用锁内版本，避免死锁
                }
            }
        } else if (s_current_game == GAME_GOMOKU) {
            // 五子棋
            if (gomoku_ui_should_exit()) {
                // 返回游戏选择菜单
                gomoku_ui_deinit();
                s_current_game = GAME_SELECTOR;
                s_game_selector_index = 0;
                s_render_requested = true;
            } else {
                gomoku_ui_update();
                if (need_render) {
                    gomoku_ui_render_locked();  // 使用锁内版本，避免死锁
                }
            }
        } else if (s_current_game == GAME_GO) {
            // 围棋
            if (go_ui_should_exit()) {
                // 返回游戏选择菜单
                go_ui_deinit();
                s_current_game = GAME_SELECTOR;
                s_game_selector_index = 0;
                s_render_requested = true;
            } else {
                go_ui_update();
                if (need_render) {
                    go_ui_render_locked();  // 使用锁内版本，避免死锁
                }
            }
        } else if (s_current_game == GAME_JUNQI) {
            // 军棋
            if (junqi_ui_should_exit()) {
                // 返回游戏选择菜单
                junqi_ui_deinit();
                s_current_game = GAME_SELECTOR;
                s_game_selector_index = 0;
                s_render_requested = true;
            } else {
                junqi_ui_update();
                if (need_render) {
                    junqi_ui_render_locked();  // 使用锁内版本，避免死锁
                }
            }
        }

        ui_lock_release();
        // ESP_LOGI(TAG, "UI update task: released lock 2");

        vTaskDelayUntil(&last_wakeup, update_delay);
    }
}

// Bluetooth chess callbacks
static void bt_chess_state_change(BluetoothChessState state)
{
    ESP_LOGI(TAG, "Bluetooth state: %d", state);
    // If entering game ready state, also request screen switch
    // 但如果是从 RECONNECTING 或 PLAYING 来的（即重连），不切换屏幕
    if (state == BT_CHESS_STATE_GAME_READY) {
        // 检查之前是否在重连或游戏中（通过检查是否有保存的游戏状态或者是否刚重连）
        // 这里简单处理：每次只有在非重连场景下才切换屏幕
        // 实际上，通过 chess_ui_switch_screen 中的检查已经足够，这里也可以再加一层
        ESP_LOGI(TAG, "Game ready! (may be reconnection)");
        // 只在非重连场景下请求切换屏幕
        // 我们通过 chess_ui_switch_screen 内部的检查来避免重置棋盘
        s_requested_screen = SCREEN_GAME;
        s_screen_switch_requested = true;
        s_render_requested = true;  // Also request render after switching
    } else {
        // Just request UI update
        s_render_requested = true;
    }
}

// 设置国际象棋退出标志
void chess_ui_set_should_exit(void)
{
    chess_ui_should_exit_flag = true;
}

static void bt_chess_device_found(const FoundBluetoothDevice* device, int index)
{
    ESP_LOGI(TAG, "Found device %d: %s", index, device->name);
    s_render_requested = true;
}

static void bt_chess_device_list_complete(int count)
{
    ESP_LOGI(TAG, "Device list complete: %d devices", count);
    s_render_requested = true;
}

static void bt_chess_connected(void)
{
    ESP_LOGI(TAG, "Bluetooth connected");
    // 如果正在重连，不切换屏幕，保持在游戏画面
    BluetoothChessState state = bt_chess_get_state();
    if (state == BT_CHESS_STATE_RECONNECTING ||
        state == BT_CHESS_STATE_GAME_READY ||
        state == BT_CHESS_STATE_PLAYING) {
        ESP_LOGI(TAG, "Reconnected - staying in game screen");
        s_render_requested = true;
        return;
    }
    // Request screen switch from UI update task
    s_requested_screen = SCREEN_BLUETOOTH_WAITING;
    s_screen_switch_requested = true;
    s_render_requested = true;  // Also request render after switching
}

static void bt_chess_disconnected(void)
{
    ESP_LOGI(TAG, "Bluetooth disconnected - staying in game, waiting for reconnection");
    // Don't exit the game - just stay in game screen and show disconnected status
    // The UI will show red status indicator
    s_render_requested = true;
}

static void bt_chess_invite_received(void)
{
    ESP_LOGI(TAG, "Bluetooth invite received");
    // 如果正在重连或者已经在游戏中，忽略这个 invite
    BluetoothChessState state = bt_chess_get_state();
    if (state == BT_CHESS_STATE_RECONNECTING ||
        state == BT_CHESS_STATE_GAME_READY ||
        state == BT_CHESS_STATE_PLAYING) {
        ESP_LOGI(TAG, "Ignoring invite - already in game or reconnecting");
        return;
    }
    // Host automatically accepts or shows prompt
    // For simplicity, let's auto-accept and send color assignment
    bt_chess_accept_connection();
}

static void bt_chess_invite_accepted(ChessColor local_color)
{
    ESP_LOGI(TAG, "Bluetooth invite accepted, local color: %d", local_color);
    // Request screen switch from UI update task
    s_requested_screen = SCREEN_GAME;
    s_screen_switch_requested = true;
    s_render_requested = true;  // Also request render after switching
}

static void bt_chess_move_received(int from_x, int from_y, int to_x, int to_y, ChessPieceType promotion)
{
    ESP_LOGI(TAG, "Bluetooth move: %d,%d -> %d,%d", from_x, from_y, to_x, to_y);
    // Queue the move for processing in UI update task
    s_move_from_x = from_x;
    s_move_from_y = from_y;
    s_move_to_x = to_x;
    s_move_to_y = to_y;
    s_move_promotion = promotion;
    s_move_pending = true;
}

static void bt_chess_game_over(ChessColor winner)
{
    ESP_LOGI(TAG, "Bluetooth game over, winner: %d", winner);
    s_render_requested = true;
}

static void bt_chess_sync_received(const ChessGame* game)
{
    ESP_LOGI(TAG, "Game sync received (BT callback context!)");
    // Queue the sync for processing in UI update task
    // 注意：我们在蓝牙回调中，不要在这里获取 ui_lock！
    // 复制数据时使用 volatile 方式或者简单的原子标记
    // 这里我们假设 ChessGame 复制是原子的（实际上不是，但在这个项目中我们用标志位保护）
    memcpy((void*)&s_synced_game, (void*)game, sizeof(ChessGame));
    s_sync_pending = true;  // 设置标志，让 UI 任务去处理
    s_render_requested = true;
}

void app_main(void)
{
    ESP_LOGI(TAG, "XiaoMiao Chess starting...");

    sound_init();
    key_init();

    ESP_LOGI(TAG, "Initialize TFT display");
    ESP_ERROR_CHECK(tft_display_init());

    ESP_LOGI(TAG, "Initialize UI lock");
    ui_lock_init();

    ESP_LOGI(TAG, "Create sprite for menu");
    if (tft_sprite_create(&s_sprite, TFT_WIDTH, TFT_HEIGHT) == ESP_OK) {
        s_sprite_created = true;
    }

    ESP_LOGI(TAG, "Initialize chess UI (will be used when selected)");
    // chess_ui_init() 会在选择游戏时调用

    xTaskCreate(key_scan_task, "KeyScan", 4096, NULL, 5, NULL);
    xTaskCreate(ui_update_task, "UIUpdate", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "Initialize bluetooth chess (will be initialized on first use)");
    // Note: bt_chess_init() will be called from the UI when entering the Bluetooth menu
    bt_chess_set_state_callback(bt_chess_state_change);
    bt_chess_set_device_found_callback(bt_chess_device_found);
    bt_chess_set_device_list_complete_callback(bt_chess_device_list_complete);
    bt_chess_set_connected_callback(bt_chess_connected);
    bt_chess_set_disconnected_callback(bt_chess_disconnected);
    bt_chess_set_invite_callback(bt_chess_invite_received);
    bt_chess_set_invite_accepted_callback(bt_chess_invite_accepted);
    bt_chess_set_move_callback(bt_chess_move_received);
    bt_chess_set_game_over_callback(bt_chess_game_over);
    bt_chess_set_sync_callback(bt_chess_sync_received);

    ESP_LOGI(TAG, "XiaoMiao Chess started!");
}
