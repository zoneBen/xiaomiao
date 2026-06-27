#include <stdio.h>
#include <unistd.h>
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
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "chess.h"
#include "chess_ui.h"
#include "chess_network.h"
#include "network.h"
#include "ui_lock.h"

static const char* TAG = "xiaomiao";

#define LCD_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

#define LCD_PIN_SCLK 18
#define LCD_PIN_MOSI 23
#define LCD_PIN_DC 4
#define LCD_PIN_RST 19
#define LCD_PIN_CS 5

#define LCD_H_RES 160
#define LCD_V_RES 128

#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8

#define LVGL_DRAW_BUF_LINES 20
#define LVGL_TICK_PERIOD_MS 2
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS 1000 / CONFIG_FREERTOS_HZ
#define LVGL_TASK_STACK_SIZE (8 * 1024)
#define LVGL_TASK_PRIORITY 2

#define BUZZER_GPIO 14
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_DUTY_RES LEDC_TIMER_13_BIT
#define LEDC_FREQUENCY 4000

#define KEY_UP_GPIO 2
#define KEY_DOWN_GPIO 13
#define KEY_LEFT_GPIO 27
#define KEY_RIGHT_GPIO 35
#define KEY_A_GPIO 34
#define KEY_B_GPIO 12

static esp_lcd_panel_io_handle_t panel_io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t* edata, void* user_ctx) {
    lv_display_t* disp = (lv_display_t*)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

static void lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;

    lv_draw_sw_rgb565_swap(px_map, (offsetx2 + 1 - offsetx1) * (offsety2 + 1 - offsety1));
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, px_map);
}

static void increase_lvgl_tick(void* arg) {
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_port_task(void* arg) {
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t time_till_next_ms = 0;
    while (1) {
        ui_lock_acquire();
        time_till_next_ms = lv_timer_handler();
        ui_lock_release();
        time_till_next_ms = MAX(time_till_next_ms, LVGL_TASK_MIN_DELAY_MS);
        time_till_next_ms = MIN(time_till_next_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}

static void buzzer_init(void) {
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

static void network_state_change(NetworkState state) {
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
}

static void chess_network_state_change(ChessNetworkState state) {
    ESP_LOGI(TAG, "Chess network state: %d", state);
}

static void chess_invite_received(const char* opponent_id) {
    ESP_LOGI(TAG, "Received invite from %s", opponent_id);
    chess_ui_show_invite_dialog(opponent_id);
}

static void chess_move_received(int from_x, int from_y, int to_x, int to_y, ChessPieceType promotion) {
    ESP_LOGI(TAG, "Received move: %d,%d -> %d,%d", from_x, from_y, to_x, to_y);
    // This callback needs to update UI with lock protection
    ui_lock_acquire();
    // TODO: Actually apply the move to the game state
    ui_lock_release();
}

void app_main(void) {
    ESP_LOGI(TAG, "XiaoMiao Chess starting...");

    buzzer_init();

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

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    lv_display_t* display = lv_display_create(LCD_H_RES, LCD_V_RES);

    size_t draw_buffer_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t);
    void* buf1 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf1);
    void* buf2 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf2);

    lv_display_set_buffers(display, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(display, panel_handle);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(panel_io_handle, &cbs, display));

    xTaskCreate(lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "Initialize network");
    network_init();
    network_set_state_callback(network_state_change);

    ESP_LOGI(TAG, "Initialize chess network");
    chess_network_init();
    chess_network_set_state_callback(chess_network_state_change);
    chess_network_set_invite_callback(chess_invite_received);
    chess_network_set_move_callback(chess_move_received);

    ESP_LOGI(TAG, "Initialize UI lock");
    ui_lock_init();

    ESP_LOGI(TAG, "Initialize chess UI");
    ui_lock_acquire();
    chess_ui_init();
    ui_lock_release();

    ESP_LOGI(TAG, "XiaoMiao Chess started!");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
