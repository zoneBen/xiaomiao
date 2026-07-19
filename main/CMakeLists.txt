idf_component_register(SRCS "xiaomiao-01.c" "chess.c" "chess_ui.c" "ui_lock.c" "sound.c" "bluetooth_game.c" "xiangqi.c" "xiangqi_ui.c" "gomoku.c" "gomoku_ui.c" "go.c" "go_ui.c" "junqi.c" "junqi_ui.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd driver esp_timer nvs_flash tft_display bt)
