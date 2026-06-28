idf_component_register(SRCS "xiaomiao-01.c" "chess.c" "chess_ui.c" "ui_lock.c" "sound.c" "chess_bluetooth.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd driver esp_timer nvs_flash tft_display bt)
