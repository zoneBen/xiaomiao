#include "boat_ui.h"

#include <stdio.h>
#include <string.h>

#include "boat_control.h"
#include "boat_draw.h"
#include "boat_link.h"
#include "boat_strings.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input.h"
#include "sound.h"
#include "tft_display.h"

static const char* TAG = "boat_ui";

#define UI_FRAME_MS       33
#define CHORD_HOLD_MS     700
#define ERROR_SHOW_MS     2000
#define SCAN_VISIBLE_ROWS 5

typedef enum {
    BOAT_SCREEN_CONTROL = 0,
    BOAT_SCREEN_SCAN,
    BOAT_SCREEN_CONNECTING
} BoatScreen;

typedef struct {
    BoatScreen screen;

    TFT_Sprite sprite;
    bool sprite_created;

    bool held[KEY_COUNT];
    bool prev[KEY_COUNT];
    bool suppress_release[KEY_COUNT];
    bool needs_sync;

    bool chord_pending;
    bool chord_fired;
    TickType_t chord_start;

    bool light_on;
    uint8_t gear;

    int selected;
    int scroll_top;
    BoatApInfo aps[BOAT_LINK_MAX_APS];
    int ap_count;
    BoatScanState scan_state;
    BoatScanState prev_scan_state;

    BoatLinkState link_state;
    BoatLinkState prev_link_state;
    char target_ssid[33];

    bool error_visible;
    TickType_t error_until;
    char error_text[24];

    bool connect_from_user;

    bool left_on;
    bool right_on;
    bool brake;
    bool drive_armed;

    uint32_t tick;
} boat_ui_t;

static boat_ui_t s_ui;

static void ui_sync_keys(void)
{
    for (int i = 0; i < KEY_COUNT; i++) {
        s_ui.prev[i] = s_ui.held[i];
        s_ui.suppress_release[i] = s_ui.held[i];
    }
    s_ui.needs_sync = false;
}

static void ui_update_scroll(void)
{
    if (s_ui.selected < s_ui.scroll_top) {
        s_ui.scroll_top = s_ui.selected;
    }
    if (s_ui.selected >= s_ui.scroll_top + SCAN_VISIBLE_ROWS) {
        s_ui.scroll_top = s_ui.selected - SCAN_VISIBLE_ROWS + 1;
    }
    if (s_ui.scroll_top < 0) {
        s_ui.scroll_top = 0;
    }
}

static void ui_show_error(BoatFailReason reason)
{
    const char* text;
    switch (reason) {
    case BOAT_FAIL_PASSWORD:
        text = STR_PASSWORD_ERROR;
        break;
    case BOAT_FAIL_NOT_FOUND:
        text = STR_NOT_FOUND;
        break;
    case BOAT_FAIL_TIMEOUT:
        text = STR_TIMEOUT;
        break;
    default:
        text = STR_CONNECT_FAIL;
        break;
    }

    strncpy(s_ui.error_text, text, sizeof(s_ui.error_text) - 1);
    s_ui.error_text[sizeof(s_ui.error_text) - 1] = '\0';
    s_ui.error_visible = true;
    s_ui.error_until = xTaskGetTickCount() + pdMS_TO_TICKS(ERROR_SHOW_MS);
}

static void ui_enter_scan(void)
{
    boat_link_stop_now();
    boat_link_start_scan();

    s_ui.screen = BOAT_SCREEN_SCAN;
    s_ui.ap_count = 0;
    s_ui.selected = 0;
    s_ui.scroll_top = 0;
    s_ui.scan_state = SCAN_RUNNING;
    s_ui.prev_scan_state = SCAN_IDLE;
    s_ui.error_visible = false;
    s_ui.error_text[0] = '\0';
    s_ui.needs_sync = true;
}

static void ui_back_to_control(void)
{
    boat_link_stop_scan();
    s_ui.screen = BOAT_SCREEN_CONTROL;
    s_ui.error_visible = false;
    s_ui.error_text[0] = '\0';
    s_ui.needs_sync = true;
    sound_play(SOUND_KEY_CLICK);
}

static void ui_connect_selected(void)
{
    if (s_ui.selected < 0 || s_ui.selected >= s_ui.ap_count) {
        return;
    }

    boat_link_stop_now();
    boat_link_connect(s_ui.aps[s_ui.selected].ssid);
    s_ui.screen = BOAT_SCREEN_CONNECTING;
    s_ui.connect_from_user = true;
    s_ui.error_visible = false;
    s_ui.needs_sync = true;
    sound_play(SOUND_MENU_SELECT);
}

static void ui_handle_control_keys(void)
{
    bool a = s_ui.held[KEY_A];
    bool b = s_ui.held[KEY_B];
    TickType_t now = xTaskGetTickCount();

    if (a && b) {
        if (!s_ui.chord_pending) {
            s_ui.chord_pending = true;
            s_ui.chord_start = now;
        } else if (!s_ui.chord_fired &&
                   (now - s_ui.chord_start) >= pdMS_TO_TICKS(CHORD_HOLD_MS)) {
            s_ui.chord_fired = true;
            sound_play(SOUND_CHORD);
            ui_enter_scan();
            return;
        }
    }

    if (s_ui.prev[KEY_A] && !a) {
        if (s_ui.suppress_release[KEY_A]) {
            s_ui.suppress_release[KEY_A] = false;
        } else if (!s_ui.chord_pending) {
            s_ui.light_on = !s_ui.light_on;
            sound_play(SOUND_MENU_SELECT);
        }
    }

    if (s_ui.prev[KEY_B] && !b) {
        if (s_ui.suppress_release[KEY_B]) {
            s_ui.suppress_release[KEY_B] = false;
        } else if (!s_ui.chord_pending) {
            s_ui.gear = boat_gear_next(s_ui.gear);
            sound_play(SOUND_MENU_SELECT);
        }
    }

    if (!a && !b) {
        s_ui.chord_pending = false;
        s_ui.chord_fired = false;
    }
}

static void ui_handle_scan_keys(void)
{
    bool a_press = s_ui.held[KEY_A] && !s_ui.prev[KEY_A];
    bool b_press = s_ui.held[KEY_B] && !s_ui.prev[KEY_B];
    bool up_press = s_ui.held[KEY_UP] && !s_ui.prev[KEY_UP];
    bool down_press = s_ui.held[KEY_DOWN] && !s_ui.prev[KEY_DOWN];

    if (a_press && b_press) {
        return;
    }

    if (a_press) {
        if (s_ui.ap_count > 0) {
            ui_connect_selected();
        } else {
            boat_link_start_scan();
            s_ui.ap_count = 0;
            s_ui.scan_state = SCAN_RUNNING;
            s_ui.error_visible = false;
            sound_play(SOUND_KEY_CLICK);
        }
        return;
    }

    if (b_press) {
        ui_back_to_control();
        return;
    }

    if (s_ui.ap_count <= 0) {
        return;
    }

    if (up_press) {
        if (s_ui.selected > 0) {
            s_ui.selected--;
            ui_update_scroll();
            sound_play(SOUND_KEY_CLICK);
        }
    } else if (down_press) {
        if (s_ui.selected < s_ui.ap_count - 1) {
            s_ui.selected++;
            ui_update_scroll();
            sound_play(SOUND_KEY_CLICK);
        }
    }
}

static void ui_handle_connecting_keys(void)
{
    if (s_ui.held[KEY_B] && !s_ui.prev[KEY_B]) {
        boat_link_cancel_connect();
        s_ui.screen = BOAT_SCREEN_SCAN;
        s_ui.connect_from_user = false;
        s_ui.needs_sync = true;
        sound_play(SOUND_KEY_CLICK);
    }
}

static void ui_poll_input(void)
{
    if (s_ui.needs_sync) {
        ui_sync_keys();
    }

    if (s_ui.screen == BOAT_SCREEN_CONTROL) {
        ui_handle_control_keys();
    } else if (s_ui.screen == BOAT_SCREEN_SCAN) {
        ui_handle_scan_keys();
    } else {
        ui_handle_connecting_keys();
    }

    if (s_ui.needs_sync) {
        ui_sync_keys();
    } else {
        for (int i = 0; i < KEY_COUNT; i++) {
            s_ui.prev[i] = s_ui.held[i];
        }
    }
}

static void ui_poll_link(void)
{
    BoatLinkState state = boat_link_get_state(s_ui.target_ssid,
                                              sizeof(s_ui.target_ssid));
    s_ui.link_state = state;

    if (state != s_ui.prev_link_state) {
        if (state == LINK_CONNECTED) {
            sound_play(SOUND_CONNECT_OK);
            if (s_ui.screen == BOAT_SCREEN_CONNECTING && s_ui.connect_from_user) {
                s_ui.screen = BOAT_SCREEN_CONTROL;
                s_ui.connect_from_user = false;
                s_ui.needs_sync = true;
                s_ui.error_visible = false;
            }
        } else if (state == LINK_CONNECT_FAILED) {
            if (s_ui.screen == BOAT_SCREEN_CONNECTING) {
                BoatFailReason reason = boat_link_get_fail_reason();
                s_ui.screen = BOAT_SCREEN_SCAN;
                s_ui.connect_from_user = false;
                s_ui.needs_sync = true;
                ui_show_error(reason);
                sound_play(SOUND_CONNECT_FAIL);
            }
        } else if (state == LINK_DISCONNECTED) {
            if (s_ui.screen == BOAT_SCREEN_CONNECTING) {
                s_ui.screen = BOAT_SCREEN_SCAN;
                s_ui.connect_from_user = false;
                s_ui.needs_sync = true;
                ui_show_error(BOAT_FAIL_OTHER);
                sound_play(SOUND_CONNECT_FAIL);
            } else if (s_ui.prev_link_state == LINK_CONNECTED &&
                       s_ui.screen == BOAT_SCREEN_CONTROL) {
                sound_play(SOUND_CONNECT_FAIL);
            }
        }
        s_ui.prev_link_state = state;
    }

    if (s_ui.error_visible &&
        (int32_t)(xTaskGetTickCount() - s_ui.error_until) >= 0) {
        s_ui.error_visible = false;
    }

    if (s_ui.screen == BOAT_SCREEN_SCAN) {
        s_ui.ap_count = boat_link_copy_aps(s_ui.aps, BOAT_LINK_MAX_APS);
        s_ui.scan_state = boat_link_get_scan_state();

        if (s_ui.scan_state == SCAN_DONE && s_ui.prev_scan_state == SCAN_RUNNING) {
            s_ui.selected = 0;
            s_ui.scroll_top = 0;
        }
        s_ui.prev_scan_state = s_ui.scan_state;

        if (s_ui.ap_count > 0) {
            if (s_ui.selected >= s_ui.ap_count) {
                s_ui.selected = s_ui.ap_count - 1;
            }
            ui_update_scroll();
        } else {
            s_ui.selected = 0;
            s_ui.scroll_top = 0;
        }
    }
}

static void ui_apply_command(void)
{
    uint8_t left = 0;
    uint8_t right = 0;
    bool brake = false;

    bool any_direction = s_ui.held[KEY_UP] || s_ui.held[KEY_DOWN] ||
                         s_ui.held[KEY_LEFT] || s_ui.held[KEY_RIGHT];

    /* Require all direction keys to be released once after entering the
     * control screen or reconnecting, so a held key cannot lurch the boat. */
    if (s_ui.screen != BOAT_SCREEN_CONTROL ||
        s_ui.link_state != LINK_CONNECTED) {
        s_ui.drive_armed = false;
    } else if (!any_direction) {
        s_ui.drive_armed = true;
    }

    if (s_ui.screen == BOAT_SCREEN_CONTROL &&
        s_ui.link_state == LINK_CONNECTED && s_ui.drive_armed) {
        boat_compute_command(s_ui.held[KEY_UP], s_ui.held[KEY_DOWN],
                             s_ui.held[KEY_LEFT], s_ui.held[KEY_RIGHT],
                             &left, &right, &brake);
    }

    boat_link_set_command(left, right, brake);
    boat_link_set_outputs(s_ui.light_on);
    boat_link_set_gear(s_ui.gear);

    s_ui.left_on = (left > 0);
    s_ui.right_on = (right > 0);
    s_ui.brake = brake;
}

static void ui_render(void)
{
    boat_view_t view = {0};
    view.link_state = s_ui.link_state;
    view.scan_state = s_ui.scan_state;
    view.light_on = s_ui.light_on;
    view.gear = s_ui.gear;
    view.left_on = s_ui.left_on;
    view.right_on = s_ui.right_on;
    view.brake = s_ui.brake;
    view.error_visible = s_ui.error_visible;
    view.error_text = s_ui.error_text;
    view.ap_count = s_ui.ap_count;
    view.selected = s_ui.selected;
    view.scroll_top = s_ui.scroll_top;
    view.aps = s_ui.aps;
    view.target_ssid = s_ui.target_ssid;
    view.tick = s_ui.tick;

    if (s_ui.screen == BOAT_SCREEN_CONTROL) {
        boat_draw_control(&s_ui.sprite, &view);
    } else if (s_ui.screen == BOAT_SCREEN_SCAN) {
        boat_draw_scan(&s_ui.sprite, &view);
    } else {
        boat_draw_connecting(&s_ui.sprite, s_ui.target_ssid, s_ui.tick);
    }

    tft_sprite_push(&s_ui.sprite, 0, 0);
}

static void boat_ui_task(void* arg)
{
    (void)arg;
    TickType_t last_wakeup = xTaskGetTickCount();

    ESP_LOGI(TAG, "UI task started");

    while (1) {
        s_ui.tick++;

        for (int i = 0; i < KEY_COUNT; i++) {
            s_ui.held[i] = input_is_held((KeyCode)i);
        }

        ui_poll_input();
        ui_poll_link();
        ui_apply_command();
        ui_render();

        vTaskDelayUntil(&last_wakeup, pdMS_TO_TICKS(UI_FRAME_MS));
    }
}

void boat_ui_init(void)
{
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.screen = BOAT_SCREEN_CONTROL;
    s_ui.gear = BOAT_GEAR_MIN;
    s_ui.scan_state = SCAN_IDLE;
    s_ui.prev_scan_state = SCAN_IDLE;

    s_ui.prev_link_state = boat_link_get_state(s_ui.target_ssid,
                                               sizeof(s_ui.target_ssid));
    s_ui.link_state = s_ui.prev_link_state;

    if (tft_sprite_create(&s_ui.sprite, TFT_WIDTH, TFT_HEIGHT) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot create framebuffer");
        return;
    }

    s_ui.sprite_created = true;
    ESP_LOGI(TAG, "UI initialized");
}

void boat_ui_start(void)
{
    if (!s_ui.sprite_created) {
        ESP_LOGE(TAG, "UI not initialized");
        return;
    }

    if (xTaskCreate(boat_ui_task, "boat_ui", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create UI task");
    }
}
