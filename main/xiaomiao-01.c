#include <stdio.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "xiaomiao";

// SPI LCD Configuration
#define LCD_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

// LCD Pins (XiaoMiao pinout)
#define LCD_PIN_SCLK 18
#define LCD_PIN_MOSI 23
#define LCD_PIN_DC 4
#define LCD_PIN_RST 19
#define LCD_PIN_CS 5

// LCD Resolution
#define LCD_H_RES 160
#define LCD_V_RES 128

// LCD Command/Param bits
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8

// LVGL Configuration
#define LVGL_DRAW_BUF_LINES 20
#define LVGL_TICK_PERIOD_MS 2
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS 1000 / CONFIG_FREERTOS_HZ
#define LVGL_TASK_STACK_SIZE (4 * 1024)
#define LVGL_TASK_PRIORITY 2

// Buzzer Configuration
#define BUZZER_GPIO 14
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_DUTY_RES LEDC_TIMER_13_BIT
#define LEDC_FREQUENCY 4000

// Button Pins
#define KEY_UP_GPIO 2
#define KEY_DOWN_GPIO 13
#define KEY_LEFT_GPIO 27
#define KEY_RIGHT_GPIO 35
#define KEY_A_GPIO 34
#define KEY_B_GPIO 12

// LVGL mutex for thread safety
static _lock_t lvgl_api_lock;

// LCD handles
static esp_lcd_panel_io_handle_t panel_io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;

// UI elements
static lv_obj_t *status_label = NULL;
static char key_text[32] = "Press any key!";

// Button states
static bool key_up_pressed(void) { return gpio_get_level(KEY_UP_GPIO) == 0; }
static bool key_down_pressed(void) { return gpio_get_level(KEY_DOWN_GPIO) == 0; }
static bool key_left_pressed(void) { return gpio_get_level(KEY_LEFT_GPIO) == 0; }
static bool key_right_pressed(void) { return gpio_get_level(KEY_RIGHT_GPIO) == 0; }
static bool key_a_pressed(void) { return gpio_get_level(KEY_A_GPIO) == 0; }
static bool key_b_pressed(void) { return gpio_get_level(KEY_B_GPIO) == 0; }

static void buzzer_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BUZZER_GPIO,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&ledc_channel);
}

static void buzzer_play_tone(uint32_t freq, uint32_t duration_ms)
{
    if (freq == 0) {
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
        return;
    }

    ledc_set_freq(LEDC_MODE, LEDC_TIMER, freq);
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, (1 << (LEDC_DUTY_RES - 1)));
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

static void keys_init(void)
{
    // Configure GPIOs with pull-up
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << KEY_UP_GPIO) | (1ULL << KEY_DOWN_GPIO) |
                        (1ULL << KEY_LEFT_GPIO) | (1ULL << KEY_B_GPIO),
        .pull_down_en = 0,
        .pull_up_en = 1
    };
    gpio_config(&io_conf);

    // GPIOs 34-39 are input-only
    gpio_config_t io_conf_no_pull = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << KEY_RIGHT_GPIO) | (1ULL << KEY_A_GPIO),
        .pull_down_en = 0,
        .pull_up_en = 0
    };
    gpio_config(&io_conf_no_pull);
}

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;

    lv_draw_sw_rgb565_swap(px_map, (offsetx2 + 1 - offsetx1) * (offsety2 + 1 - offsety1));
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, px_map);
}

static void increase_lvgl_tick(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t time_till_next_ms = 0;
    while (1) {
        _lock_acquire(&lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        _lock_release(&lvgl_api_lock);
        time_till_next_ms = MAX(time_till_next_ms, LVGL_TASK_MIN_DELAY_MS);
        time_till_next_ms = MIN(time_till_next_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}

static void create_ui(lv_display_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);

    // Set background color
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    // Title label
    lv_obj_t *title_label = lv_label_create(scr);
    lv_label_set_text(title_label, "XiaoMiao");
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);

    // Status label - save reference for updates
    status_label = lv_label_create(scr);
    lv_label_set_text(status_label, key_text);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFFFFF), 0);

    // Key labels
    lv_obj_t *keys_label = lv_label_create(scr);
    lv_label_set_text(keys_label, "U D L R A B");
    lv_obj_align(keys_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_text_color(keys_label, lv_color_hex(0x888888), 0);
}

void app_main(void)
{
    ESP_LOGI(TAG, "XiaoMiao starting...");

    // Initialize buzzer
    buzzer_init();

    // Initialize keys
    keys_init();

    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &panel_io_handle));

    ESP_LOGI(TAG, "Install ST7789 panel driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));

    // Set screen gap for ST7789
    //esp_lcd_panel_set_gap(panel_handle, 40, 40);

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    // Create LVGL display
    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);

    // Allocate draw buffers
    size_t draw_buffer_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t);
    void *buf1 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf1);
    void *buf2 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf2);

    lv_display_set_buffers(display, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(display, panel_handle);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    // Install LVGL tick timer
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    // Register event callback for flush ready
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(panel_io_handle, &cbs, display));

    // Create LVGL task
    xTaskCreate(lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    // Create UI
    _lock_acquire(&lvgl_api_lock);
    create_ui(display);
    _lock_release(&lvgl_api_lock);

    ESP_LOGI(TAG, "XiaoMiao started!");

    bool last_state = false;

    while (1) {
        bool any_key = key_up_pressed() || key_down_pressed() ||
                       key_left_pressed() || key_right_pressed() ||
                       key_a_pressed() || key_b_pressed();

        if (any_key && !last_state) {
            const char *key_name = "Unknown";
            uint32_t tone_freq = 0;

            if (key_up_pressed()) {
                key_name = "UP";
                tone_freq = 523;
            } else if (key_down_pressed()) {
                key_name = "DOWN";
                tone_freq = 587;
            } else if (key_left_pressed()) {
                key_name = "LEFT";
                tone_freq = 659;
            } else if (key_right_pressed()) {
                key_name = "RIGHT";
                tone_freq = 698;
            } else if (key_a_pressed()) {
                key_name = "A";
                tone_freq = 784;
            } else if (key_b_pressed()) {
                key_name = "B";
                tone_freq = 880;
            }

            // Play tone
            buzzer_play_tone(tone_freq, 100);

            // Update screen display - with LVGL lock
            if (status_label) {
                _lock_acquire(&lvgl_api_lock);
                snprintf(key_text, sizeof(key_text), "Pressed: %s!", key_name);
                lv_label_set_text(status_label, key_text);
                _lock_release(&lvgl_api_lock);
            }

            ESP_LOGI(TAG, "Key pressed: %s", key_name);
        }

        last_state = any_key;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
