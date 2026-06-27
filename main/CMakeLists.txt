idf_component_register(SRCS "xiaomiao-01.c" "chess.c" "network.c" "chess_network.c" "chess_ui.c" "ui_lock.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd driver esp_timer esp_adc esp_wifi mqtt nvs_flash json tft_display)
