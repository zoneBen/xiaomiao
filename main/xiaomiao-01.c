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

static const char* TAG = "xiaomiao";

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
                        chess_ui_handle_key(keys[i].key);
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
    bool need_render = false;
    ChessScreen requested_screen = SCREEN_MAIN_MENU;
    int move_from_x = 0, move_from_y = 0, move_to_x = 0, move_to_y = 0;
    ChessPieceType move_promotion = PIECE_NONE;

    while (1) {
        // Step 1: Check and collect requests without holding the lock for too long
        ui_lock_acquire();

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

        need_render = s_render_requested;
        if (need_render) {
            s_render_requested = false;
        }

        ui_lock_release();

        // Step 2: Process requests - hold the lock while modifying UI state
        ui_lock_acquire();

        if (need_screen_switch) {
            ESP_LOGI(TAG, "Switching screen to %d from UI task", requested_screen);
            chess_ui_switch_screen(requested_screen);
        }

        if (need_apply_move) {
            ESP_LOGI(TAG, "Applying opponent move from UI task");
            chess_ui_apply_opponent_move_locked(move_from_x, move_from_y, move_to_x, move_to_y, move_promotion);
        }

        if (need_screen_switch) {
            // Force render when switching screens to make sure new screen appears immediately
            chess_ui_render_locked_force();
        } else if (need_render) {
            chess_ui_render_locked();
        }

        // Regular UI update
        chess_ui_update();

        ui_lock_release();

        vTaskDelayUntil(&last_wakeup, update_delay);
    }
}

// Bluetooth chess callbacks
static void bt_chess_state_change(BluetoothChessState state)
{
    ESP_LOGI(TAG, "Bluetooth state: %d", state);
    // If entering game ready state, also request screen switch
    if (state == BT_CHESS_STATE_GAME_READY) {
        ESP_LOGI(TAG, "Game ready! Requesting screen switch to game");
        s_requested_screen = SCREEN_GAME;
        s_screen_switch_requested = true;
        s_render_requested = true;  // Also request render after switching
    } else {
        // Just request UI update
        s_render_requested = true;
    }
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

void app_main(void)
{
    ESP_LOGI(TAG, "XiaoMiao Chess starting...");

    sound_init();
    key_init();

    ESP_LOGI(TAG, "Initialize TFT display");
    ESP_ERROR_CHECK(tft_display_init());

    ESP_LOGI(TAG, "Initialize UI lock");
    ui_lock_init();

    ESP_LOGI(TAG, "Initialize chess UI");
    ui_lock_acquire();
    chess_ui_init();
    ui_lock_release();

    chess_ui_render();

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

    ESP_LOGI(TAG, "XiaoMiao Chess started!");
}
