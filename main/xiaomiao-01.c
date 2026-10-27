#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"

#include "boat_link.h"
#include "boat_ui.h"
#include "input.h"
#include "sound.h"
#include "tft_display.h"

static const char* TAG = "boat";

void app_main(void)
{
    ESP_LOGI(TAG, "XC_BOT remote starting");

    sound_init();
    input_init();

    ESP_ERROR_CHECK(tft_display_init());

    if (!boat_link_init()) {
        ESP_LOGE(TAG, "WiFi link init failed, UI will stay disconnected");
    }

    boat_ui_init();
    input_start();
    boat_ui_start();

    ESP_LOGI(TAG, "Ready, free heap: %u bytes",
             (unsigned)esp_get_free_heap_size());
}
