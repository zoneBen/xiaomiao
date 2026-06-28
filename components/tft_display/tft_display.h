#ifndef TFT_DISPLAY_H
#define TFT_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Display configuration
#define TFT_WIDTH 160
#define TFT_HEIGHT 128

// Helper macro to check if a string contains Chinese characters
#define IS_CN_CHAR(c) ((uint8_t)(c) >= 0x80)


// Color definitions (RGB565)
#define TFT_BLACK       0x0000
#define TFT_NAVY        0x000F
#define TFT_DARKGREEN   0x03E0
#define TFT_DARKCYAN    0x03EF
#define TFT_MAROON      0x7800
#define TFT_PURPLE      0x780F
#define TFT_OLIVE       0x7BE0
#define TFT_LIGHTGREY   0xC618
#define TFT_DARKGREY    0x7BE0
#define TFT_BLUE        0x001F
#define TFT_GREEN       0x07E0
#define TFT_CYAN        0x07FF
#define TFT_RED         0xF800
#define TFT_MAGENTA     0xF81F
#define TFT_YELLOW      0xFFE0
#define TFT_WHITE       0xFFFF
#define TFT_ORANGE      0xFDA0
#define TFT_GREENYELLOW 0xB7E0
#define TFT_PINK        0xFE19
#define TFT_BROWN       0x9A60
#define TFT_GOLD        0xFEA0
#define TFT_SILVER      0xC618
#define TFT_SKYBLUE     0x867D
#define TFT_VIOLET      0x915C

// Chess board colors
#define COLOR_LIGHT_SQUARE RGB_TO_RGB565(0x8B, 0x73, 0x55)  // Darker beige
#define COLOR_DARK_SQUARE  RGB_TO_RGB565(0x3D, 0x23, 0x0A)  // Very dark brown
#define COLOR_BOARD_BG     RGB_TO_RGB565(0x2A, 0x2A, 0x2A)  // Dark gray board background
#define COLOR_WHITE_PIECE  TFT_WHITE
#define COLOR_BLACK_PIECE  TFT_BLACK
#define COLOR_CURSOR       0x07FF    // Cyan for cursor
#define COLOR_SELECTED     0x07E0    // Green for selected
#define COLOR_LAST_MOVE    RGB_TO_RGB565(0x40, 0xA0, 0x40)  // Green highlight for last move
#define COLOR_BACKGROUND   TFT_BLACK
#define COLOR_DIALOG       RGB_TO_RGB565(0x1A, 0x1A, 0x1A)  // Dark gray dialog
#define COLOR_TEXT         TFT_WHITE

// Convert RGB888 to RGB565
#define RGB_TO_RGB565(r, g, b) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))

// Sprite structure for double buffering
typedef struct {
    int16_t width;
    int16_t height;
    uint16_t* buffer;
    bool created;
} TFT_Sprite;

// Function prototypes
esp_err_t tft_display_init(void);
void tft_display_deinit(void);

// Basic drawing functions
void tft_draw_pixel(int16_t x, int16_t y, uint16_t color);
void tft_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
void tft_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void tft_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void tft_fill_screen(uint16_t color);

// Sprite functions
esp_err_t tft_sprite_create(TFT_Sprite* sprite, int16_t width, int16_t height);
void tft_sprite_delete(TFT_Sprite* sprite);
void tft_sprite_draw_pixel(TFT_Sprite* sprite, int16_t x, int16_t y, uint16_t color);
void tft_sprite_fill_rect(TFT_Sprite* sprite, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void tft_sprite_fill_screen(TFT_Sprite* sprite, uint16_t color);
void tft_sprite_push(TFT_Sprite* sprite, int16_t x, int16_t y);
void tft_sprite_push_from_screen(TFT_Sprite* sprite, int16_t x, int16_t y);
void tft_sprite_draw_rect(TFT_Sprite* sprite, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

// XBitmap drawing (for chess pieces)
void tft_sprite_draw_xbitmap(TFT_Sprite* sprite, int16_t x, int16_t y, const uint8_t* bitmap,
                             int16_t w, int16_t h, uint16_t color, uint16_t bg_color);
void tft_sprite_draw_char(TFT_Sprite* sprite, int16_t x, int16_t y, char c, uint16_t color, uint8_t size);
int16_t tft_sprite_text_width(const char* str, uint8_t size);

// Text functions
void tft_set_text_color(uint16_t color);
void tft_set_text_size(uint8_t size);
void tft_set_cursor(int16_t x, int16_t y);
size_t tft_print_char(char c);
size_t tft_print(const char* str);
void tft_draw_string(int16_t x, int16_t y, const char* str, uint16_t color, uint8_t size);
void tft_draw_char(int16_t x, int16_t y, char c, uint16_t color, uint8_t size);
int16_t tft_text_width(const char* str, uint8_t size);
int16_t tft_font_height(uint8_t size);

// Push entire buffer
void tft_push_buffer(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t* data);

#ifdef __cplusplus
}
#endif

#endif  // TFT_DISPLAY_H
