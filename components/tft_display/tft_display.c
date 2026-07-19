#include "tft_display.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "TFT";

// SPI configuration
#define LCD_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (26 * 1000 * 1000)

#define LCD_PIN_SCLK 18
#define LCD_PIN_MOSI 23
#define LCD_PIN_DC 4
#define LCD_PIN_RST 19
#define LCD_PIN_CS 5

#define LCD_H_RES TFT_WIDTH
#define LCD_V_RES TFT_HEIGHT

#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8

static esp_lcd_panel_io_handle_t panel_io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;
static spi_bus_config_t buscfg;

// 6x8 font data (ASCII 32-127)
static const uint8_t font6x8[96][6] = {
    {0x00,0x00,0x00,0x00,0x00,0x00}, // space
    {0x00,0x00,0x5F,0x00,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14,0x00}, // #
    {0x24,0x2A,0x7F,0x2A,0x12,0x00}, // $
    {0x23,0x13,0x08,0x64,0x62,0x00}, // %
    {0x36,0x49,0x55,0x22,0x50,0x00}, // &
    {0x00,0x05,0x03,0x00,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00,0x00}, // )
    {0x14,0x08,0x3E,0x08,0x14,0x00}, // *
    {0x08,0x08,0x3E,0x08,0x08,0x00}, // +
    {0x00,0x50,0x30,0x00,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08,0x00}, // -
    {0x00,0x60,0x60,0x00,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02,0x00}, // /
    {0x3E,0x51,0x49,0x45,0x3E,0x00}, // 0
    {0x00,0x42,0x7F,0x40,0x00,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46,0x00}, // 2
    {0x21,0x41,0x45,0x4B,0x31,0x00}, // 3
    {0x18,0x14,0x12,0x7F,0x10,0x00}, // 4
    {0x27,0x45,0x45,0x45,0x39,0x00}, // 5
    {0x3C,0x4A,0x49,0x49,0x30,0x00}, // 6
    {0x01,0x71,0x09,0x05,0x03,0x00}, // 7
    {0x36,0x49,0x49,0x49,0x36,0x00}, // 8
    {0x06,0x49,0x49,0x29,0x1E,0x00}, // 9
    {0x00,0x36,0x36,0x00,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00,0x00}, // ;
    {0x08,0x14,0x22,0x41,0x00,0x00}, // <
    {0x14,0x14,0x14,0x14,0x14,0x00}, // =
    {0x00,0x41,0x22,0x14,0x08,0x00}, // >
    {0x02,0x01,0x51,0x09,0x06,0x00}, // ?
    {0x32,0x49,0x79,0x41,0x3E,0x00}, // @
    {0x7E,0x11,0x11,0x11,0x7E,0x00}, // A
    {0x7F,0x49,0x49,0x49,0x36,0x00}, // B
    {0x3E,0x41,0x41,0x41,0x22,0x00}, // C
    {0x7F,0x41,0x41,0x41,0x3E,0x00}, // D
    {0x7F,0x49,0x49,0x49,0x41,0x00}, // E
    {0x7F,0x09,0x09,0x09,0x01,0x00}, // F
    {0x3E,0x41,0x51,0x51,0x7A,0x00}, // G
    {0x7F,0x08,0x08,0x08,0x7F,0x00}, // H
    {0x00,0x41,0x7F,0x41,0x00,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01,0x00}, // J
    {0x7F,0x08,0x14,0x22,0x41,0x00}, // K
    {0x7F,0x40,0x40,0x40,0x40,0x00}, // L
    {0x7F,0x02,0x0C,0x02,0x7F,0x00}, // M
    {0x7F,0x04,0x08,0x10,0x7F,0x00}, // N
    {0x3E,0x41,0x41,0x41,0x3E,0x00}, // O
    {0x7F,0x09,0x09,0x09,0x06,0x00}, // P
    {0x3E,0x41,0x51,0x21,0x5E,0x00}, // Q
    {0x7F,0x09,0x19,0x29,0x46,0x00}, // R
    {0x46,0x49,0x49,0x49,0x31,0x00}, // S
    {0x01,0x01,0x7F,0x01,0x01,0x00}, // T
    {0x3F,0x40,0x40,0x40,0x3F,0x00}, // U
    {0x1F,0x20,0x40,0x20,0x1F,0x00}, // V
    {0x3F,0x40,0x38,0x40,0x3F,0x00}, // W
    {0x63,0x14,0x08,0x14,0x63,0x00}, // X
    {0x07,0x08,0x70,0x08,0x07,0x00}, // Y
    {0x61,0x51,0x49,0x45,0x43,0x00}, // Z
    {0x00,0x7F,0x41,0x41,0x00,0x00}, // [
    {0x02,0x04,0x08,0x10,0x20,0x00}, // backslash
    {0x00,0x41,0x41,0x7F,0x00,0x00}, // ]
    {0x04,0x02,0x01,0x02,0x04,0x00}, // ^
    {0x40,0x40,0x40,0x40,0x40,0x00}, // _
    {0x00,0x01,0x02,0x04,0x00,0x00}, // `
    {0x20,0x54,0x54,0x54,0x78,0x00}, // a
    {0x7F,0x48,0x44,0x44,0x38,0x00}, // b
    {0x38,0x44,0x44,0x44,0x20,0x00}, // c
    {0x38,0x44,0x44,0x48,0x7F,0x00}, // d
    {0x38,0x54,0x54,0x54,0x18,0x00}, // e
    {0x08,0x7E,0x09,0x01,0x02,0x00}, // f
    {0x08,0x14,0x54,0x54,0x3C,0x00}, // g
    {0x7F,0x08,0x04,0x04,0x78,0x00}, // h
    {0x00,0x44,0x7D,0x40,0x00,0x00}, // i
    {0x20,0x40,0x44,0x3D,0x00,0x00}, // j
    {0x7F,0x10,0x28,0x44,0x00,0x00}, // k
    {0x00,0x41,0x7F,0x40,0x00,0x00}, // l
    {0x7C,0x04,0x18,0x04,0x78,0x00}, // m
    {0x7C,0x08,0x04,0x04,0x78,0x00}, // n
    {0x38,0x44,0x44,0x44,0x38,0x00}, // o
    {0x7C,0x14,0x14,0x14,0x08,0x00}, // p
    {0x08,0x14,0x14,0x18,0x7C,0x00}, // q
    {0x7C,0x08,0x04,0x04,0x08,0x00}, // r
    {0x48,0x54,0x54,0x54,0x24,0x00}, // s
    {0x04,0x3F,0x44,0x40,0x20,0x00}, // t
    {0x3C,0x40,0x40,0x20,0x7C,0x00}, // u
    {0x1C,0x20,0x40,0x20,0x1C,0x00}, // v
    {0x3C,0x40,0x30,0x40,0x3C,0x00}, // w
    {0x44,0x28,0x10,0x28,0x44,0x00}, // x
    {0x0C,0x50,0x50,0x50,0x3C,0x00}, // y
    {0x44,0x64,0x54,0x4C,0x44,0x00}, // z
    {0x00,0x08,0x36,0x41,0x00,0x00}, // {
    {0x00,0x00,0x7F,0x00,0x00,0x00}, // |
    {0x00,0x41,0x36,0x08,0x00,0x00}, // }
    {0x10,0x08,0x08,0x10,0x08,0x00}, // ~
    {0x00,0x00,0x00,0x00,0x00,0x00}  // del
};

esp_err_t tft_display_init(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Initialize SPI bus");
    buscfg.sclk_io_num = LCD_PIN_SCLK;
    buscfg.mosi_io_num = LCD_PIN_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
    ret = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed");
        return ret;
    }

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
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &panel_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel IO init failed");
        return ret;
    }

    ESP_LOGI(TAG, "Install ST7735/ST7789 panel driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7789(panel_io_handle, &panel_config, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel driver init failed");
        return ret;
    }

    ret = esp_lcd_panel_reset(panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel reset failed");
        return ret;
    }

    ret = esp_lcd_panel_init(panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel init failed");
        return ret;
    }

    ret = esp_lcd_panel_invert_color(panel_handle, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel invert color failed");
        return ret;
    }

    ret = esp_lcd_panel_swap_xy(panel_handle, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel swap xy failed");
        return ret;
    }

    ret = esp_lcd_panel_mirror(panel_handle, true, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel mirror failed");
        return ret;
    }

    ret = esp_lcd_panel_disp_on_off(panel_handle, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel display on failed");
        return ret;
    }

    ESP_LOGI(TAG, "TFT display initialized");
    return ESP_OK;
}

void tft_display_deinit(void)
{
    if (panel_handle) {
        esp_lcd_panel_del(panel_handle);
        panel_handle = NULL;
    }
    if (panel_io_handle) {
        esp_lcd_panel_io_del(panel_io_handle);
        panel_io_handle = NULL;
    }
    spi_bus_free(LCD_HOST);
}

void tft_draw_pixel(int16_t x, int16_t y, uint16_t color)
{
    if (x < 0 || x >= TFT_WIDTH || y < 0 || y >= TFT_HEIGHT) return;

    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + 1, y + 1, &color);
}

void tft_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    int16_t dx = abs(x1 - x0);
    int16_t dy = abs(y1 - y0);
    int16_t sx = x0 < x1 ? 1 : -1;
    int16_t sy = y0 < y1 ? 1 : -1;
    int16_t err = dx - dy;

    while (1) {
        tft_draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void tft_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    tft_draw_line(x, y, x + w - 1, y, color);
    tft_draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    tft_draw_line(x, y, x, y + h - 1, color);
    tft_draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

void tft_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > TFT_WIDTH) w = TFT_WIDTH - x;
    if (y + h > TFT_HEIGHT) h = TFT_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    int16_t buffer_size = w * h;
    uint16_t* buffer = (uint16_t*)malloc(buffer_size * sizeof(uint16_t));
    if (!buffer) return;

    for (int i = 0; i < buffer_size; i++) {
        buffer[i] = color;
    }

    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, buffer);
    free(buffer);
}

void tft_fill_screen(uint16_t color)
{
    tft_fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, color);
}

void tft_push_buffer(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t* data)
{
    if (x < 0 || y < 0 || x + w > TFT_WIDTH || y + h > TFT_HEIGHT) return;
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, data);
}

// Sprite functions
esp_err_t tft_sprite_create(TFT_Sprite* sprite, int16_t width, int16_t height)
{
    if (!sprite) return ESP_ERR_INVALID_ARG;

    sprite->width = width;
    sprite->height = height;
    sprite->buffer = (uint16_t*)malloc(width * height * sizeof(uint16_t));
    if (!sprite->buffer) {
        ESP_LOGE(TAG, "Failed to allocate sprite buffer");
        return ESP_ERR_NO_MEM;
    }
    sprite->created = true;

    tft_sprite_fill_screen(sprite, TFT_BLACK);
    return ESP_OK;
}

void tft_sprite_delete(TFT_Sprite* sprite)
{
    if (sprite && sprite->buffer) {
        free(sprite->buffer);
        sprite->buffer = NULL;
        sprite->created = false;
    }
}

static inline void sprite_set_pixel(TFT_Sprite* sprite, int16_t x, int16_t y, uint16_t color)
{
    if (x >= 0 && x < sprite->width && y >= 0 && y < sprite->height) {
        sprite->buffer[y * sprite->width + x] = color;
    }
}

void tft_sprite_draw_pixel(TFT_Sprite* sprite, int16_t x, int16_t y, uint16_t color)
{
    if (!sprite || !sprite->created) return;
    sprite_set_pixel(sprite, x, y, color);
}

void tft_sprite_fill_rect(TFT_Sprite* sprite, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    if (!sprite || !sprite->created) return;

    for (int16_t yy = y; yy < y + h; yy++) {
        for (int16_t xx = x; xx < x + w; xx++) {
            sprite_set_pixel(sprite, xx, yy, color);
        }
    }
}

void tft_sprite_draw_line(TFT_Sprite* sprite, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    if (!sprite || !sprite->created) return;

    int16_t dx = abs(x1 - x0);
    int16_t dy = abs(y1 - y0);
    int16_t sx = x0 < x1 ? 1 : -1;
    int16_t sy = y0 < y1 ? 1 : -1;
    int16_t err = dx - dy;

    while (1) {
        sprite_set_pixel(sprite, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void tft_sprite_draw_hline(TFT_Sprite* sprite, int16_t x, int16_t y, int16_t w, uint16_t color)
{
    if (!sprite || !sprite->created) return;

    for (int16_t i = 0; i < w; i++) {
        sprite_set_pixel(sprite, x + i, y, color);
    }
}

void tft_sprite_draw_string(TFT_Sprite* sprite, int16_t x, int16_t y, const char* str, uint16_t color, uint8_t size)
{
    if (!sprite || !sprite->created) return;

    int16_t orig_x = x;
    while (*str) {
        if (*str == '\n') {
            y += 8 * size;
            x = orig_x;
        } else {
            tft_sprite_draw_char(sprite, x, y, *str, color, size);
            x += 6 * size;
        }
        str++;
    }
}

void tft_sprite_draw_circle(TFT_Sprite* sprite, int16_t x0, int16_t y0, int16_t r, uint16_t color)
{
    if (!sprite || !sprite->created) return;

    int16_t f = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t x = 0;
    int16_t y = r;

    sprite_set_pixel(sprite, x0, y0 + r, color);
    sprite_set_pixel(sprite, x0, y0 - r, color);
    sprite_set_pixel(sprite, x0 + r, y0, color);
    sprite_set_pixel(sprite, x0 - r, y0, color);

    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;

        sprite_set_pixel(sprite, x0 + x, y0 + y, color);
        sprite_set_pixel(sprite, x0 - x, y0 + y, color);
        sprite_set_pixel(sprite, x0 + x, y0 - y, color);
        sprite_set_pixel(sprite, x0 - x, y0 - y, color);
        sprite_set_pixel(sprite, x0 + y, y0 + x, color);
        sprite_set_pixel(sprite, x0 - y, y0 + x, color);
        sprite_set_pixel(sprite, x0 + y, y0 - x, color);
        sprite_set_pixel(sprite, x0 - y, y0 - x, color);
    }
}

void tft_sprite_draw_rect(TFT_Sprite* sprite, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    if (!sprite || !sprite->created) return;

    // Draw top and bottom lines
    for (int16_t i = 0; i < w; i++) {
        sprite_set_pixel(sprite, x + i, y, color);
        sprite_set_pixel(sprite, x + i, y + h - 1, color);
    }

    // Draw left and right lines
    for (int16_t i = 0; i < h; i++) {
        sprite_set_pixel(sprite, x, y + i, color);
        sprite_set_pixel(sprite, x + w - 1, y + i, color);
    }
}

void tft_sprite_fill_screen(TFT_Sprite* sprite, uint16_t color)
{
    if (!sprite || !sprite->created) return;

    int32_t size = sprite->width * sprite->height;
    for (int32_t i = 0; i < size; i++) {
        sprite->buffer[i] = color;
    }
}

void tft_sprite_push(TFT_Sprite* sprite, int16_t x, int16_t y)
{
    if (!sprite || !sprite->created) return;
    tft_push_buffer(x, y, sprite->width, sprite->height, sprite->buffer);
}

void tft_sprite_push_from_screen(TFT_Sprite* sprite, int16_t x, int16_t y)
{
    // Not implemented for now - would need read functionality
    tft_sprite_push(sprite, x, y);
}

void tft_sprite_draw_xbitmap(TFT_Sprite* sprite, int16_t x, int16_t y, const uint8_t* bitmap,
                             int16_t w, int16_t h, uint16_t color, uint16_t bg_color)
{
    if (!sprite || !sprite->created) return;

    int16_t byteWidth = (w + 7) / 8;
    uint8_t byte = 0;

    for (int16_t yy = 0; yy < h; yy++) {
        for (int16_t xx = 0; xx < w; xx++) {
            if (xx & 7) byte <<= 1;
            else byte = bitmap[yy * byteWidth + xx / 8];

            if (byte & 0x80) {
                sprite_set_pixel(sprite, x + xx, y + yy, color);
            } else {
                sprite_set_pixel(sprite, x + xx, y + yy, bg_color);
            }
        }
    }
}

void tft_sprite_draw_char(TFT_Sprite* sprite, int16_t x, int16_t y, char c, uint16_t color, uint8_t size)
{
    if (!sprite || !sprite->created) return;
    if (c < 32 || c > 127) c = 127;

    const uint8_t* glyph = font6x8[c - 32];

    for (int16_t yy = 0; yy < 8; yy++) {
        for (int16_t xx = 0; xx < 6; xx++) {
            if (glyph[xx] & (1 << yy)) {
                for (uint8_t sy = 0; sy < size; sy++) {
                    for (uint8_t sx = 0; sx < size; sx++) {
                        sprite_set_pixel(sprite, x + xx * size + sx, y + yy * size + sy, color);
                    }
                }
            }
        }
    }
}

int16_t tft_sprite_text_width(const char* str, uint8_t size)
{
    int16_t len = 0;
    while (*str++) {
        len += 6 * size;
    }
    return len;
}

// Text functions - screen (not sprite)
static uint16_t text_color = TFT_WHITE;
static uint8_t text_size = 1;
static int16_t cursor_x = 0;
static int16_t cursor_y = 0;

void tft_set_text_color(uint16_t color)
{
    text_color = color;
}

void tft_set_text_size(uint8_t size)
{
    text_size = size > 0 ? size : 1;
}

void tft_set_cursor(int16_t x, int16_t y)
{
    cursor_x = x;
    cursor_y = y;
}

void tft_draw_char(int16_t x, int16_t y, char c, uint16_t color, uint8_t size)
{
    if (c < 32 || c > 127) c = 127;

    const uint8_t* glyph = font6x8[c - 32];

    for (int16_t yy = 0; yy < 8; yy++) {
        for (int16_t xx = 0; xx < 6; xx++) {
            if (glyph[xx] & (1 << yy)) {
                for (uint8_t sy = 0; sy < size; sy++) {
                    for (uint8_t sx = 0; sx < size; sx++) {
                        tft_draw_pixel(x + xx * size + sx, y + yy * size + sy, color);
                    }
                }
            }
        }
    }
}

size_t tft_print_char(char c)
{
    if (c == '\n') {
        cursor_y += 8 * text_size;
        cursor_x = 0;
    } else if (c == '\r') {
        cursor_x = 0;
    } else {
        tft_draw_char(cursor_x, cursor_y, c, text_color, text_size);
        cursor_x += 6 * text_size;
    }
    return 1;
}

size_t tft_print(const char* str)
{
    size_t len = 0;
    while (*str) {
        tft_print_char(*str++);
        len++;
    }
    return len;
}

void tft_draw_string(int16_t x, int16_t y, const char* str, uint16_t color, uint8_t size)
{
    int16_t orig_x = x;
    uint16_t orig_color = text_color;
    uint8_t orig_size = text_size;

    text_color = color;
    text_size = size;

    while (*str) {
        if (*str == '\n') {
            y += 8 * size;
            x = orig_x;
        } else {
            tft_draw_char(x, y, *str, color, size);
            x += 6 * size;
        }
        str++;
    }

    text_color = orig_color;
    text_size = orig_size;
}

int16_t tft_text_width(const char* str, uint8_t size)
{
    int16_t len = 0;
    while (*str++) {
        len += 6 * size;
    }
    return len;
}

int16_t tft_font_height(uint8_t size)
{
    return 8 * size;
}
