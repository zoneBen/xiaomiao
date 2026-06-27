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
#include "chess_network.h"
#include "network.h"
#include "ui_lock.h"
#include "tft_display.h"
#include "sound.h"

static const char* TAG = "xiaomiao";


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
    const TickType_t update_delay = pdMS_TO_TICKS(100);

    ESP_LOGI(TAG, "UI update task started");

    while (1) {
        ui_lock_acquire();
        chess_ui_update();
        ui_lock_release();

        vTaskDelayUntil(&last_wakeup, update_delay);
    }
}

static void network_state_change(NetworkState state)
{
    const char* status_str = "Unknown";
    switch (state) {
        case NETWORK_STATE_DISCONNECTED: status_str = "Status: Disconnected"; break;
        case NETWORK_STATE_CONNECTING_WIFI: status_str = "Status: Connecting WiFi..."; break;
        case NETWORK_STATE_WIFI_CONNECTED: status_str = "Status: WiFi Connected"; break;
        case NETWORK_STATE_CONNECTING_MQTT: status_str = "Status: Connecting MQTT..."; break;
        case NETWORK_STATE_MQTT_CONNECTED: status_str = "Status: Connected"; break;
        case NETWORK_STATE_ERROR: status_str = "Status: Error"; break;
    }
    chess_ui_set_network_status(status_str);
    chess_ui_render();
}

static void chess_network_state_change(ChessNetworkState state)
{
    ESP_LOGI(TAG, "Chess network state: %d", state);
}

static void chess_invite_received(const char* opponent_id)
{
    ESP_LOGI(TAG, "Received invite from %s", opponent_id);
    chess_ui_show_invite_dialog(opponent_id);
}

static void chess_move_received(int from_x, int from_y, int to_x, int to_y, ChessPieceType promotion)
{
    ESP_LOGI(TAG, "Received move: %d,%d -> %d,%d", from_x, from_y, to_x, to_y);
    ui_lock_acquire();
    ui_lock_release();
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

    ESP_LOGI(TAG, "Initialize network");
    network_init();
    network_set_state_callback(network_state_change);

    ESP_LOGI(TAG, "Initialize chess network");
    chess_network_init();
    chess_network_set_state_callback(chess_network_state_change);
    chess_network_set_invite_callback(chess_invite_received);
    chess_network_set_move_callback(chess_move_received);

    ESP_LOGI(TAG, "XiaoMiao Chess started!");
}
