#include "input.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "input";

#define KEY_UP_GPIO    2
#define KEY_DOWN_GPIO  13
#define KEY_LEFT_GPIO  27
#define KEY_RIGHT_GPIO 35
#define KEY_A_GPIO     34
#define KEY_B_GPIO     12

#define KEY_SCAN_MS    10
#define KEY_DEBOUNCE_MS 25

typedef struct {
    int gpio;
    KeyCode key;
    const char* name;
    bool raw_last;
    bool stable;
    TickType_t raw_change_tick;
} key_state_t;

static key_state_t s_keys[KEY_COUNT] = {
    {KEY_UP_GPIO,    KEY_UP,    "UP",    false, false, 0},
    {KEY_DOWN_GPIO,  KEY_DOWN,  "DOWN",  false, false, 0},
    {KEY_LEFT_GPIO,  KEY_LEFT,  "LEFT",  false, false, 0},
    {KEY_RIGHT_GPIO, KEY_RIGHT, "RIGHT", false, false, 0},
    {KEY_A_GPIO,     KEY_A,     "A",     false, false, 0},
    {KEY_B_GPIO,     KEY_B,     "B",     false, false, 0},
};

static volatile bool s_held[KEY_COUNT];
static portMUX_TYPE s_held_lock = portMUX_INITIALIZER_UNLOCKED;

void input_init(void)
{
    /* GPIO 2/13/27/12 have internal pull-ups. */
    gpio_config_t pullup_conf = {0};
    pullup_conf.intr_type = GPIO_INTR_DISABLE;
    pullup_conf.mode = GPIO_MODE_INPUT;
    pullup_conf.pin_bit_mask = (1ULL << KEY_UP_GPIO) |
                               (1ULL << KEY_DOWN_GPIO) |
                               (1ULL << KEY_LEFT_GPIO) |
                               (1ULL << KEY_B_GPIO);
    pullup_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&pullup_conf));

    /* GPIO 34/35 are input-only and use external pull-ups. */
    gpio_config_t input_only_conf = {0};
    input_only_conf.intr_type = GPIO_INTR_DISABLE;
    input_only_conf.mode = GPIO_MODE_INPUT;
    input_only_conf.pin_bit_mask = (1ULL << KEY_RIGHT_GPIO) |
                                   (1ULL << KEY_A_GPIO);
    input_only_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    input_only_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&input_only_conf));

    ESP_LOGI(TAG, "Buttons initialized (active-low)");
}

static void input_task(void* arg)
{
    (void)arg;
    TickType_t last_wakeup = xTaskGetTickCount();
    const TickType_t debounce_ticks = pdMS_TO_TICKS(KEY_DEBOUNCE_MS);

    ESP_LOGI(TAG, "Key scan task started");

    while (1) {
        TickType_t now = xTaskGetTickCount();

        for (int i = 0; i < KEY_COUNT; i++) {
            key_state_t* k = &s_keys[i];
            bool raw = (gpio_get_level(k->gpio) == 0);

            if (raw != k->raw_last) {
                k->raw_last = raw;
                k->raw_change_tick = now;
            }

            if ((now - k->raw_change_tick) >= debounce_ticks && k->stable != raw) {
                k->stable = raw;
                portENTER_CRITICAL(&s_held_lock);
                s_held[k->key] = raw;
                portEXIT_CRITICAL(&s_held_lock);
                ESP_LOGD(TAG, "Key %s %s", k->name, raw ? "pressed" : "released");
            }
        }

        vTaskDelayUntil(&last_wakeup, pdMS_TO_TICKS(KEY_SCAN_MS));
    }
}

void input_start(void)
{
    if (xTaskCreate(input_task, "input", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create input task");
    }
}

bool input_is_held(KeyCode key)
{
    if (key >= KEY_COUNT) {
        return false;
    }

    portENTER_CRITICAL(&s_held_lock);
    bool held = s_held[key];
    portEXIT_CRITICAL(&s_held_lock);
    return held;
}
