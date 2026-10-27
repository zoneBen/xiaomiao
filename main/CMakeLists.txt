idf_component_register(
    SRCS "xiaomiao-01.c" "input.c" "boat_link.c" "boat_storage.c" "boat_udp.c"
         "boat_ui.c" "boat_draw.c" "sound.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_timer nvs_flash tft_display esp_wifi esp_netif esp_event lwip)
