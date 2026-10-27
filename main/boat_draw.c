#include "boat_draw.h"

#include <stdio.h>
#include <string.h>

#include "boat_strings.h"

#define C_WATER      RGB_TO_RGB565(6, 28, 56)
#define C_WATER_LINE RGB_TO_RGB565(20, 80, 120)
#define C_PANEL      RGB_TO_RGB565(8, 12, 20)
#define C_HULL       RGB_TO_RGB565(208, 216, 224)
#define C_HULL_EDGE  RGB_TO_RGB565(86, 98, 112)
#define C_DECK       RGB_TO_RGB565(150, 172, 192)
#define C_GLASS      RGB_TO_RGB565(60, 190, 220)
#define C_THRUST_OFF RGB_TO_RGB565(70, 78, 88)
#define C_THRUST_ON  RGB_TO_RGB565(40, 230, 120)
#define C_LAMP_ON    RGB_TO_RGB565(255, 220, 40)
#define C_LAMP_DIM   RGB_TO_RGB565(130, 105, 20)
#define C_LAMP_OFF   RGB_TO_RGB565(95, 95, 75)
#define C_BUZZER_ON  RGB_TO_RGB565(80, 220, 255)
#define C_BUZZER_OFF RGB_TO_RGB565(90, 100, 110)

#define BOAT_CX 80

static int utf8_width(const char* text, uint8_t scale)
{
    return tft_sprite_utf8_width(text, scale);
}

static void draw_text(TFT_Sprite* sprite, int x, int y, const char* text,
                      uint16_t color, uint8_t scale)
{
    tft_sprite_draw_utf8(sprite, x, y, text, color, scale);
}

static void draw_center(TFT_Sprite* sprite, int y, const char* text,
                        uint16_t color, uint8_t scale)
{
    int width = utf8_width(text, scale);
    draw_text(sprite, (TFT_WIDTH - width) / 2, y, text, color, scale);
}

static void draw_right(TFT_Sprite* sprite, int y, const char* text,
                       uint16_t color, uint8_t scale)
{
    int width = utf8_width(text, scale);
    draw_text(sprite, TFT_WIDTH - 4 - width, y, text, color, scale);
}

static const char* link_text(BoatLinkState state)
{
    switch (state) {
    case LINK_CONNECTED:
        return STR_STATUS_CONNECTED;
    case LINK_CONNECTING:
        return STR_STATUS_CONNECTING;
    default:
        return STR_STATUS_DISCONNECTED;
    }
}

static uint16_t link_color(BoatLinkState state)
{
    switch (state) {
    case LINK_CONNECTED:
        return TFT_GREEN;
    case LINK_CONNECTING:
        return TFT_YELLOW;
    default:
        return TFT_RED;
    }
}

static void draw_rssi_bars(TFT_Sprite* sprite, int x, int y, int8_t rssi,
                           uint16_t on_color, uint16_t off_color)
{
    int level;
    if (rssi >= -55) {
        level = 4;
    } else if (rssi >= -65) {
        level = 3;
    } else if (rssi >= -75) {
        level = 2;
    } else if (rssi >= -85) {
        level = 1;
    } else {
        level = 0;
    }

    for (int i = 0; i < 4; i++) {
        int height = 3 + i * 2;
        tft_sprite_fill_rect(sprite, x + i * 4, y + 9 - height, 3, height,
                             i < level ? on_color : off_color);
    }
}

static void draw_status_bar(TFT_Sprite* sprite, const boat_view_t* view)
{
    tft_sprite_fill_rect(sprite, 0, 0, TFT_WIDTH, 18, TFT_NAVY);
    draw_text(sprite, 4, 1, STR_APP_TITLE, TFT_WHITE, 2);
    draw_right(sprite, 1, link_text(view->link_state),
               link_color(view->link_state), 2);
    if (view->brake) {
        draw_center(sprite, 1, STR_BRAKE, TFT_RED, 2);
    }
}

static void draw_water(TFT_Sprite* sprite, uint32_t tick)
{
    tft_sprite_fill_rect(sprite, 0, 18, TFT_WIDTH, 76, C_WATER);

    for (int i = 0; i < 3; i++) {
        int y = 28 + i * 22;
        int offset = (int)((tick / 2 + (uint32_t)i * 19) % 60);
        for (int x = -offset; x < TFT_WIDTH; x += 60) {
            tft_sprite_draw_hline(sprite, (int16_t)x, (int16_t)y, 18, C_WATER_LINE);
        }
    }
}

static int hull_half_width(int y)
{
    if (y < 24 || y > 88) {
        return 0;
    }

    int t = y - 24;
    if (t < 14) {
        return 1 + (t * 19) / 14;
    }
    if (t < 54) {
        return 20;
    }
    if (t < 64) {
        return 20 - ((t - 54) * 6) / 10;
    }
    return 14;
}

static void draw_hull(TFT_Sprite* sprite)
{
    for (int y = 24; y <= 88; y++) {
        int half = hull_half_width(y);
        if (half <= 0) {
            continue;
        }

        tft_sprite_draw_hline(sprite, (int16_t)(BOAT_CX - half), (int16_t)y,
                              (int16_t)(half * 2), C_HULL);
        int inner = half - 3;
        if (inner > 0) {
            tft_sprite_draw_hline(sprite, (int16_t)(BOAT_CX - inner), (int16_t)y,
                                  (int16_t)(inner * 2), C_DECK);
        }
    }

    for (int y = 24; y <= 88; y++) {
        int half = hull_half_width(y);
        if (half <= 0) {
            continue;
        }
        tft_sprite_draw_pixel(sprite, (int16_t)(BOAT_CX - half), (int16_t)y,
                              C_HULL_EDGE);
        tft_sprite_draw_pixel(sprite, (int16_t)(BOAT_CX + half - 1), (int16_t)y,
                              C_HULL_EDGE);
    }
}

static void draw_cabin(TFT_Sprite* sprite)
{
    tft_sprite_fill_rect(sprite, 67, 50, 26, 18, C_HULL_EDGE);
    tft_sprite_fill_rect(sprite, 68, 51, 24, 16, C_DECK);
    tft_sprite_fill_rect(sprite, 70, 53, 20, 6, C_GLASS);
}

static void draw_lamp(TFT_Sprite* sprite, bool on)
{
    if (on) {
        tft_sprite_draw_circle(sprite, BOAT_CX, 30, 7, C_LAMP_DIM);
        tft_sprite_fill_circle(sprite, BOAT_CX, 30, 4, C_LAMP_ON);
    } else {
        tft_sprite_fill_circle(sprite, BOAT_CX, 30, 3, C_LAMP_OFF);
    }
}

static void draw_buzzer(TFT_Sprite* sprite, bool on, uint32_t tick)
{
    uint16_t color = on ? C_BUZZER_ON : C_BUZZER_OFF;

    tft_sprite_fill_rect(sprite, 73, 41, 6, 6, color);
    tft_sprite_draw_line(sprite, 79, 41, 84, 44, color);
    tft_sprite_draw_line(sprite, 79, 46, 84, 44, color);
    tft_sprite_fill_rect(sprite, 84, 43, 2, 2, color);

    if (on) {
        int phase = (int)((tick / 4) % 2);
        for (int i = 0; i < 3; i++) {
            int x = 87 + i * 3;
            int span = 2 + i + phase;
            tft_sprite_draw_line(sprite, (int16_t)x, (int16_t)(44 - span),
                                 (int16_t)x, (int16_t)(44 + span), color);
        }
    }
}

static const int8_t s_prop_dirs[12][2] = {
    {5, 0}, {4, 3}, {3, 4}, {0, 5}, {-3, 4}, {-4, 3},
    {-5, 0}, {-4, -3}, {-3, -4}, {0, -5}, {3, -4}, {4, -3},
};

static void draw_thruster(TFT_Sprite* sprite, int cx, int cy, bool on, uint32_t tick)
{
    uint16_t blade_color = on ? C_THRUST_ON : C_HULL_EDGE;

    if (on) {
        tft_sprite_draw_circle(sprite, (int16_t)cx, (int16_t)cy, 8, C_THRUST_ON);
    }

    tft_sprite_fill_rect(sprite, (int16_t)(cx - 5), (int16_t)(cy - 5), 10, 10,
                         C_THRUST_OFF);
    tft_sprite_draw_rect(sprite, (int16_t)(cx - 5), (int16_t)(cy - 5), 10, 10,
                         C_HULL_EDGE);

    int base = (int)((tick / 2) % 12);
    for (int i = 0; i < 3; i++) {
        int idx = (base + i * 4) % 12;
        tft_sprite_draw_line(sprite, (int16_t)cx, (int16_t)cy,
                             (int16_t)(cx + s_prop_dirs[idx][0]),
                             (int16_t)(cy + s_prop_dirs[idx][1]), blade_color);
    }
    tft_sprite_fill_circle(sprite, (int16_t)cx, (int16_t)cy, 1, blade_color);

    if (on) {
        int len = 4 + (int)((tick / 3) % 5);
        tft_sprite_draw_hline(sprite, (int16_t)(cx - 3), (int16_t)(cy + 7), len,
                              C_WATER_LINE);
        tft_sprite_draw_hline(sprite, (int16_t)(cx + 1), (int16_t)(cy + 9), len,
                              C_WATER_LINE);
    }
}

static void draw_brake(TFT_Sprite* sprite)
{
    tft_sprite_draw_hline(sprite, 52, 91, 20, TFT_RED);
    tft_sprite_draw_hline(sprite, 88, 91, 20, TFT_RED);
    tft_sprite_draw_hline(sprite, 56, 93, 18, TFT_RED);
    tft_sprite_draw_hline(sprite, 86, 93, 18, TFT_RED);
}

static void draw_boat(TFT_Sprite* sprite, const boat_view_t* view)
{
    draw_hull(sprite);
    draw_cabin(sprite);
    draw_lamp(sprite, view->light_on);
    draw_buzzer(sprite, view->buzzer_on, view->tick);
    draw_thruster(sprite, 68, 84, view->left_on, view->tick);
    draw_thruster(sprite, 92, 84, view->right_on, view->tick);
    if (view->brake) {
        draw_brake(sprite);
    }
}

static void draw_bottom_bar(TFT_Sprite* sprite, const boat_view_t* view)
{
    tft_sprite_fill_rect(sprite, 0, 94, TFT_WIDTH, 34, C_PANEL);
    tft_sprite_draw_hline(sprite, 0, 94, TFT_WIDTH, C_HULL_EDGE);

    const char* light = view->light_on ? STR_LIGHT_ON : STR_LIGHT_OFF;
    const char* buzzer = view->buzzer_on ? STR_BUZZER_ON : STR_BUZZER_OFF;
    draw_text(sprite, 4, 94, light, view->light_on ? TFT_YELLOW : TFT_LIGHTGREY, 2);
    draw_text(sprite, 84, 94, buzzer, view->buzzer_on ? TFT_CYAN : TFT_LIGHTGREY, 2);

    uint16_t hint_color = (view->link_state == LINK_CONNECTED) ? TFT_DARKGREY : TFT_RED;
    draw_center(sprite, 110, STR_HOLD_SCAN, hint_color, 2);
}

void boat_draw_control(TFT_Sprite* sprite, const boat_view_t* view)
{
    tft_sprite_fill_screen(sprite, C_WATER);
    draw_water(sprite, view->tick);
    draw_boat(sprite, view);
    draw_status_bar(sprite, view);
    draw_bottom_bar(sprite, view);
}

static void draw_scan_top(TFT_Sprite* sprite, const boat_view_t* view)
{
    tft_sprite_fill_rect(sprite, 0, 0, TFT_WIDTH, 18, TFT_NAVY);
    draw_text(sprite, 4, 1, STR_SELECT_BOAT, TFT_WHITE, 2);

    if (view->scan_state == SCAN_RUNNING) {
        draw_right(sprite, 1, STR_SEARCHING, TFT_YELLOW, 2);
    } else {
        char count[24];
        snprintf(count, sizeof(count), STR_AP_COUNT_FMT, view->ap_count);
        draw_right(sprite, 1, count, TFT_LIGHTGREY, 2);
    }
}

static void draw_scan_rows(TFT_Sprite* sprite, const boat_view_t* view)
{
    for (int i = 0; i < 5; i++) {
        int index = view->scroll_top + i;
        if (index >= view->ap_count) {
            break;
        }

        int y = 20 + i * 16;
        bool selected = (index == view->selected);
        if (selected) {
            tft_sprite_fill_rect(sprite, 2, y, TFT_WIDTH - 4, 15, TFT_NAVY);
        }

        char ssid[20];
        int n = 0;
        while (view->aps[index].ssid[n] != '\0' && n < 18) {
            ssid[n] = view->aps[index].ssid[n];
            n++;
        }
        ssid[n] = '\0';

        draw_text(sprite, 6, y + 4, ssid, selected ? TFT_YELLOW : TFT_WHITE, 1);
        draw_rssi_bars(sprite, 132, y + 2, view->aps[index].rssi,
                       selected ? TFT_YELLOW : TFT_GREEN, C_THRUST_OFF);
    }
}

static void draw_scan_empty(TFT_Sprite* sprite, const boat_view_t* view)
{
    if (view->scan_state == SCAN_RUNNING) {
        char text[32];
        snprintf(text, sizeof(text), "%s...", STR_SEARCHING);
        draw_center(sprite, 44, text, TFT_YELLOW, 2);
    } else {
        draw_center(sprite, 44, STR_NOT_FOUND, TFT_LIGHTGREY, 2);
    }
}

static void draw_scan_footer(TFT_Sprite* sprite, const boat_view_t* view)
{
    tft_sprite_fill_rect(sprite, 0, 100, TFT_WIDTH, 28, C_PANEL);
    tft_sprite_draw_hline(sprite, 0, 100, TFT_WIDTH, C_HULL_EDGE);

    if (view->error_visible && view->error_text != NULL && view->error_text[0] != '\0') {
        draw_center(sprite, 106, view->error_text, TFT_RED, 2);
        return;
    }

    const char* left = (view->ap_count > 0) ? STR_A_CONNECT : STR_A_RETRY_SEARCH;
    draw_text(sprite, 20, 106, left, TFT_YELLOW, 2);
    draw_text(sprite, 84, 106, STR_B_BACK, TFT_LIGHTGREY, 2);
}

void boat_draw_scan(TFT_Sprite* sprite, const boat_view_t* view)
{
    tft_sprite_fill_screen(sprite, C_PANEL);
    draw_scan_top(sprite, view);
    if (view->ap_count > 0) {
        draw_scan_rows(sprite, view);
    } else {
        draw_scan_empty(sprite, view);
    }
    draw_scan_footer(sprite, view);
}

void boat_draw_connecting(TFT_Sprite* sprite, const char* ssid, uint32_t tick)
{
    tft_sprite_fill_screen(sprite, C_PANEL);
    draw_center(sprite, 30, STR_CONNECTING, TFT_WHITE, 2);

    char name[21];
    int n = 0;
    if (ssid != NULL) {
        while (ssid[n] != '\0' && n < 20) {
            name[n] = ssid[n];
            n++;
        }
    }
    name[n] = '\0';
    int name_width = utf8_width(name, 1);
    draw_text(sprite, (TFT_WIDTH - name_width) / 2, 58, name, TFT_LIGHTGREY, 1);

    char dots[5] = "....";
    dots[(tick / 10) % 4] = '\0';
    draw_center(sprite, 76, dots, TFT_YELLOW, 2);

    draw_center(sprite, 96, STR_B_CANCEL, TFT_LIGHTGREY, 2);
}
