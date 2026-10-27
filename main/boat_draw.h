#ifndef BOAT_DRAW_H
#define BOAT_DRAW_H

#include <stdbool.h>
#include <stdint.h>

#include "boat_link.h"
#include "tft_display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    BoatLinkState link_state;
    BoatScanState scan_state;

    bool light_on;
    bool buzzer_on;
    bool left_on;
    bool right_on;
    bool brake;

    bool error_visible;
    const char* error_text;

    int ap_count;
    int selected;
    int scroll_top;
    const BoatApInfo* aps;

    const char* target_ssid;
    uint32_t tick;
} boat_view_t;

void boat_draw_control(TFT_Sprite* sprite, const boat_view_t* view);
void boat_draw_scan(TFT_Sprite* sprite, const boat_view_t* view);
void boat_draw_connecting(TFT_Sprite* sprite, const char* ssid, uint32_t tick);

#ifdef __cplusplus
}
#endif

#endif /* BOAT_DRAW_H */
