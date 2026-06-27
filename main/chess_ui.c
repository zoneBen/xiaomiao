#include "chess_ui.h"
#include "chess_network.h"
#include "network.h"
#include "ui_lock.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "chess_ui";

typedef struct {
    lv_obj_t* screen_main;
    lv_obj_t* screen_network;
    lv_obj_t* screen_lobby;
    lv_obj_t* screen_game;

    lv_obj_t* ssid_input;
    lv_obj_t* password_input;
    lv_obj_t* mqtt_uri_input;
    lv_obj_t* player_id_label;
    lv_obj_t* network_status_label;
    lv_obj_t* connect_btn;
    lv_obj_t* disconnect_btn;

    lv_obj_t* lobby_status_label;
    lv_obj_t* search_btn;
    lv_obj_t* stop_search_btn;
    lv_obj_t* opponent_list;

    lv_obj_t* board_container;
    lv_obj_t* board_squares[8][8];
    lv_obj_t* turn_label;
    lv_obj_t* move_history;
    lv_obj_t* resign_btn;
    lv_obj_t* draw_btn;
    lv_obj_t* back_btn;

    lv_obj_t* invite_dialog;
    lv_obj_t* invite_label;
    lv_obj_t* accept_btn;
    lv_obj_t* reject_btn;
    char invite_opponent_id[32];

    ChessPiece selected_piece;
    int selected_x;
    int selected_y;
    bool has_selection;

    ChessScreen current_screen;
} ChessUIContext;

static ChessUIContext* g_ui = NULL;
static ChessGame* g_game = NULL;
static lv_group_t* g_group = NULL;

static const char* piece_symbols[2][6] = {
    {"♙", "♘", "♗", "♖", "♕", "♔"},
    {"♟", "♞", "♝", "♜", "♛", "♚"}
};

static void msgbox_timer_cb(lv_timer_t* t) {
    lv_obj_t* m = (lv_obj_t*)lv_timer_get_user_data(t);
    if (lv_obj_is_valid(m)) {
        lv_msgbox_close(m);
    }
}

static void update_board_display(void) {
    if (!g_ui || !g_game) {
        return;
    }

    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            ChessPiece piece = chess_game_get_piece(g_game, x, y);
            lv_obj_t* btn = g_ui->board_squares[x][y];
            lv_obj_t* label = lv_obj_get_child(btn, 0);

            if (g_ui->has_selection && g_ui->selected_x == x && g_ui->selected_y == y) {
                lv_obj_set_style_bg_color(btn, lv_color_hex(0x00FF00), 0);
            } else if ((x + y) % 2 == 0) {
                lv_obj_set_style_bg_color(btn, lv_color_hex(0xF0D9B5), 0);
            } else {
                lv_obj_set_style_bg_color(btn, lv_color_hex(0xB58863), 0);
            }

            if (piece.type != PIECE_NONE) {
                lv_label_set_text(label, piece_symbols[piece.color][piece.type - 1]);
            } else {
                lv_label_set_text(label, "");
            }
        }
    }

    ChessColor turn = chess_game_get_current_turn(g_game);
    if (g_ui->turn_label) {
        lv_label_set_text_fmt(g_ui->turn_label, "Turn: %s",
                             turn == COLOR_WHITE ? "White" : "Black");
    }
}

static void board_square_event_cb(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    int x = (int)(intptr_t)lv_obj_get_user_data(btn);
    int y = (x >> 8) & 0xFF;
    x = x & 0xFF;

    ESP_LOGI(TAG, "Square clicked: %d, %d", x, y);

    if (!g_ui || !g_game) {
        return;
    }

    ChessPiece clicked_piece = chess_game_get_piece(g_game, x, y);

    if (g_ui->has_selection) {
        if (g_ui->selected_x == x && g_ui->selected_y == y) {
            g_ui->has_selection = false;
            g_ui->selected_x = -1;
            g_ui->selected_y = -1;
        } else if (chess_game_is_valid_move(g_game, g_ui->selected_x, g_ui->selected_y, x, y)) {
            if (chess_game_make_move(g_game, g_ui->selected_x, g_ui->selected_y, x, y, PIECE_QUEEN)) {
                if (chess_network_get_state() == CHESS_NET_STATE_PLAYING) {
                    chess_network_send_move(g_ui->selected_x, g_ui->selected_y, x, y, PIECE_QUEEN);
                }
            }
            g_ui->has_selection = false;
            g_ui->selected_x = -1;
            g_ui->selected_y = -1;
        } else if (clicked_piece.type != PIECE_NONE &&
                   clicked_piece.color == chess_game_get_current_turn(g_game)) {
            g_ui->selected_x = x;
            g_ui->selected_y = y;
        } else {
            g_ui->has_selection = false;
            g_ui->selected_x = -1;
            g_ui->selected_y = -1;
        }
    } else {
        if (clicked_piece.type != PIECE_NONE &&
            clicked_piece.color == chess_game_get_current_turn(g_game)) {
            g_ui->selected_x = x;
            g_ui->selected_y = y;
            g_ui->has_selection = true;
        }
    }

    update_board_display();
}

static void btn_local_game_cb(lv_event_t* e) {
    chess_ui_switch_screen(SCREEN_GAME);
    chess_game_reset(g_game);
    update_board_display();
}

static void btn_network_game_cb(lv_event_t* e) {
    chess_ui_switch_screen(SCREEN_NETWORK_SETUP);
}

static void btn_connect_cb(lv_event_t* e) {
    const char* ssid = lv_textarea_get_text(g_ui->ssid_input);
    const char* pass = lv_textarea_get_text(g_ui->password_input);
    const char* mqtt_uri = lv_textarea_get_text(g_ui->mqtt_uri_input);

    if (strlen(ssid) > 0) {
        network_set_wifi_credentials(ssid, pass);
        network_set_mqtt_config(mqtt_uri, chess_network_get_player_id(), "", "");
        network_connect();
    }
}

static void btn_disconnect_cb(lv_event_t* e) {
    network_disconnect();
}

static void btn_network_back_cb(lv_event_t* e) {
    if (network_is_connected()) {
        chess_ui_switch_screen(SCREEN_LOBBY);
    } else {
        chess_ui_switch_screen(SCREEN_MAIN_MENU);
    }
}

static void btn_search_cb(lv_event_t* e) {
    if (chess_network_start_search()) {
        lv_label_set_text(g_ui->lobby_status_label, "Searching for opponent...");
        lv_obj_add_flag(g_ui->search_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_ui->stop_search_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void btn_stop_search_cb(lv_event_t* e) {
    chess_network_stop_search();
    lv_label_set_text(g_ui->lobby_status_label, "Search stopped");
    lv_obj_clear_flag(g_ui->search_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ui->stop_search_btn, LV_OBJ_FLAG_HIDDEN);
}

static void btn_lobby_back_cb(lv_event_t* e) {
    chess_network_stop_search();
    chess_ui_switch_screen(SCREEN_NETWORK_SETUP);
}

static void btn_game_back_cb(lv_event_t* e) {
    chess_network_send_resign();
    if (network_is_connected()) {
        chess_ui_switch_screen(SCREEN_LOBBY);
    } else {
        chess_ui_switch_screen(SCREEN_MAIN_MENU);
    }
}

static void btn_resign_cb(lv_event_t* e) {
    chess_network_send_resign();
}

static void btn_draw_cb(lv_event_t* e) {
    chess_network_send_draw_offer();
}

static void btn_accept_invite_cb(lv_event_t* e) {
    if (g_ui && g_ui->invite_opponent_id[0] != '\0') {
        chess_network_accept_invite(g_ui->invite_opponent_id);
        chess_ui_switch_screen(SCREEN_GAME);
        chess_game_reset(g_game);
        update_board_display();
    }
    chess_ui_hide_invite_dialog();
}

static void btn_reject_invite_cb(lv_event_t* e) {
    if (g_ui && g_ui->invite_opponent_id[0] != '\0') {
        chess_network_reject_invite(g_ui->invite_opponent_id);
    }
    chess_ui_hide_invite_dialog();
}

static void create_main_menu_screen(void) {
    g_ui->screen_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_ui->screen_main, lv_color_hex(0x1a1a2e), 0);

    lv_obj_t* title = lv_label_create(g_ui->screen_main);
    lv_label_set_text(title, "♔ Chess ♚");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t* btn_local = lv_btn_create(g_ui->screen_main);
    lv_obj_set_size(btn_local, 140, 50);
    lv_obj_align(btn_local, LV_ALIGN_CENTER, 0, -30);
    lv_obj_add_flag(btn_local, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, btn_local);
    lv_obj_t* label_local = lv_label_create(btn_local);
    lv_label_set_text(label_local, "Local Game");
    lv_obj_center(label_local);

    lv_obj_t* btn_network = lv_btn_create(g_ui->screen_main);
    lv_obj_set_size(btn_network, 140, 50);
    lv_obj_align(btn_network, LV_ALIGN_CENTER, 0, 40);
    lv_obj_add_flag(btn_network, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, btn_network);
    lv_obj_t* label_network = lv_label_create(btn_network);
    lv_label_set_text(label_network, "Network Game");
    lv_obj_center(label_network);

    lv_obj_add_event_cb(btn_local, btn_local_game_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_network, btn_network_game_cb, LV_EVENT_CLICKED, NULL);
}

static void create_network_setup_screen(void) {
    g_ui->screen_network = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_ui->screen_network, lv_color_hex(0x1a1a2e), 0);

    lv_obj_t* title = lv_label_create(g_ui->screen_network);
    lv_label_set_text(title, "Network Setup");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t* ssid_label = lv_label_create(g_ui->screen_network);
    lv_label_set_text(ssid_label, "WiFi SSID:");
    lv_obj_set_style_text_color(ssid_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ssid_label, LV_ALIGN_TOP_LEFT, 10, 40);

    g_ui->ssid_input = lv_textarea_create(g_ui->screen_network);
    lv_obj_set_size(g_ui->ssid_input, 140, 35);
    lv_obj_align(g_ui->ssid_input, LV_ALIGN_TOP_LEFT, 10, 60);
    lv_textarea_set_placeholder_text(g_ui->ssid_input, "SSID");
    lv_textarea_set_one_line(g_ui->ssid_input, true);
    lv_obj_add_flag(g_ui->ssid_input, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, g_ui->ssid_input);

    lv_obj_t* pass_label = lv_label_create(g_ui->screen_network);
    lv_label_set_text(pass_label, "Password:");
    lv_obj_set_style_text_color(pass_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(pass_label, LV_ALIGN_TOP_LEFT, 10, 100);

    g_ui->password_input = lv_textarea_create(g_ui->screen_network);
    lv_obj_set_size(g_ui->password_input, 140, 35);
    lv_obj_align(g_ui->password_input, LV_ALIGN_TOP_LEFT, 10, 120);
    lv_textarea_set_placeholder_text(g_ui->password_input, "Password");
    lv_textarea_set_password_mode(g_ui->password_input, true);
    lv_textarea_set_one_line(g_ui->password_input, true);
    lv_obj_add_flag(g_ui->password_input, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, g_ui->password_input);

    lv_obj_t* mqtt_label = lv_label_create(g_ui->screen_network);
    lv_label_set_text(mqtt_label, "MQTT Broker:");
    lv_obj_set_style_text_color(mqtt_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(mqtt_label, LV_ALIGN_TOP_LEFT, 10, 160);

    g_ui->mqtt_uri_input = lv_textarea_create(g_ui->screen_network);
    lv_obj_set_size(g_ui->mqtt_uri_input, 140, 35);
    lv_obj_align(g_ui->mqtt_uri_input, LV_ALIGN_TOP_LEFT, 10, 180);
    lv_textarea_set_placeholder_text(g_ui->mqtt_uri_input, "mqtt://broker.hivemq.com");
    lv_textarea_set_one_line(g_ui->mqtt_uri_input, true);
    lv_obj_add_flag(g_ui->mqtt_uri_input, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, g_ui->mqtt_uri_input);

    lv_textarea_set_text(g_ui->mqtt_uri_input, "mqtt://broker.hivemq.com");

    g_ui->player_id_label = lv_label_create(g_ui->screen_network);
    lv_label_set_text_fmt(g_ui->player_id_label, "ID: %s", chess_network_get_player_id());
    lv_obj_set_style_text_color(g_ui->player_id_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(g_ui->player_id_label, LV_ALIGN_TOP_LEFT, 10, 220);

    g_ui->network_status_label = lv_label_create(g_ui->screen_network);
    lv_label_set_text(g_ui->network_status_label, "Status: Disconnected");
    lv_obj_set_style_text_color(g_ui->network_status_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(g_ui->network_status_label, LV_ALIGN_TOP_LEFT, 10, 240);

    g_ui->connect_btn = lv_btn_create(g_ui->screen_network);
    lv_obj_set_size(g_ui->connect_btn, 60, 35);
    lv_obj_align(g_ui->connect_btn, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_add_flag(g_ui->connect_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, g_ui->connect_btn);
    lv_obj_t* label_connect = lv_label_create(g_ui->connect_btn);
    lv_label_set_text(label_connect, "Connect");
    lv_obj_center(label_connect);

    g_ui->disconnect_btn = lv_btn_create(g_ui->screen_network);
    lv_obj_set_size(g_ui->disconnect_btn, 60, 35);
    lv_obj_align(g_ui->disconnect_btn, LV_ALIGN_BOTTOM_LEFT, 80, -10);
    lv_obj_add_flag(g_ui->disconnect_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, g_ui->disconnect_btn);
    lv_obj_t* label_disconnect = lv_label_create(g_ui->disconnect_btn);
    lv_label_set_text(label_disconnect, "Disconn");
    lv_obj_center(label_disconnect);

    lv_obj_t* btn_back = lv_btn_create(g_ui->screen_network);
    lv_obj_set_size(btn_back, 60, 35);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_add_flag(btn_back, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, btn_back);
    lv_obj_t* label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, "Back");
    lv_obj_center(label_back);

    lv_obj_add_event_cb(g_ui->connect_btn, btn_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(g_ui->disconnect_btn, btn_disconnect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_back, btn_network_back_cb, LV_EVENT_CLICKED, NULL);
}

static void create_lobby_screen(void) {
    g_ui->screen_lobby = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_ui->screen_lobby, lv_color_hex(0x1a1a2e), 0);

    lv_obj_t* title = lv_label_create(g_ui->screen_lobby);
    lv_label_set_text(title, "Game Lobby");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    g_ui->lobby_status_label = lv_label_create(g_ui->screen_lobby);
    lv_label_set_text(g_ui->lobby_status_label, "Status: Waiting");
    lv_obj_set_style_text_color(g_ui->lobby_status_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(g_ui->lobby_status_label, LV_ALIGN_TOP_MID, 0, 35);

    g_ui->search_btn = lv_btn_create(g_ui->screen_lobby);
    lv_obj_set_size(g_ui->search_btn, 100, 35);
    lv_obj_align(g_ui->search_btn, LV_ALIGN_TOP_LEFT, 10, 60);
    lv_obj_add_flag(g_ui->search_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, g_ui->search_btn);
    lv_obj_t* label_search = lv_label_create(g_ui->search_btn);
    lv_label_set_text(label_search, "Search");
    lv_obj_center(label_search);

    g_ui->stop_search_btn = lv_btn_create(g_ui->screen_lobby);
    lv_obj_set_size(g_ui->stop_search_btn, 100, 35);
    lv_obj_align(g_ui->stop_search_btn, LV_ALIGN_TOP_LEFT, 120, 60);
    lv_obj_add_flag(g_ui->stop_search_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, g_ui->stop_search_btn);
    lv_obj_t* label_stop = lv_label_create(g_ui->stop_search_btn);
    lv_label_set_text(label_stop, "Stop");
    lv_obj_center(label_stop);
    lv_obj_add_flag(g_ui->stop_search_btn, LV_OBJ_FLAG_HIDDEN);

    g_ui->opponent_list = lv_list_create(g_ui->screen_lobby);
    lv_obj_set_size(g_ui->opponent_list, 140, 100);
    lv_obj_align(g_ui->opponent_list, LV_ALIGN_TOP_LEFT, 10, 100);
    lv_obj_add_flag(g_ui->opponent_list, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, g_ui->opponent_list);

    lv_obj_t* btn_back = lv_btn_create(g_ui->screen_lobby);
    lv_obj_set_size(btn_back, 60, 35);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_add_flag(btn_back, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, btn_back);
    lv_obj_t* label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, "Back");
    lv_obj_center(label_back);

    lv_obj_add_event_cb(g_ui->search_btn, btn_search_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(g_ui->stop_search_btn, btn_stop_search_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_back, btn_lobby_back_cb, LV_EVENT_CLICKED, NULL);
}

static void create_game_screen(void) {
    g_ui->screen_game = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_ui->screen_game, lv_color_hex(0x1a1a2e), 0);

    g_ui->turn_label = lv_label_create(g_ui->screen_game);
    lv_label_set_text(g_ui->turn_label, "Turn: White");
    lv_obj_set_style_text_color(g_ui->turn_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(g_ui->turn_label, LV_ALIGN_TOP_MID, 0, 5);

    g_ui->board_container = lv_obj_create(g_ui->screen_game);
    lv_obj_set_size(g_ui->board_container, 128, 128);
    lv_obj_align(g_ui->board_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(g_ui->board_container, 2, 0);
    lv_obj_set_style_bg_color(g_ui->board_container, lv_color_hex(0x000000), 0);
    lv_obj_set_layout(g_ui->board_container, LV_LAYOUT_GRID);

    static lv_coord_t col_dsc[] = {16, 16, 16, 16, 16, 16, 16, 16, LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc[] = {16, 16, 16, 16, 16, 16, 16, 16, LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(g_ui->board_container, col_dsc, row_dsc);

    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            lv_obj_t* btn = lv_btn_create(g_ui->board_container);
            lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, y, 1, LV_GRID_ALIGN_STRETCH, x, 1);
            lv_obj_set_style_radius(btn, 0, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_add_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
            lv_group_add_obj(g_group, btn);

            if ((x + y) % 2 == 0) {
                lv_obj_set_style_bg_color(btn, lv_color_hex(0xF0D9B5), 0);
            } else {
                lv_obj_set_style_bg_color(btn, lv_color_hex(0xB58863), 0);
            }

            int user_data = (x & 0xFF) | ((y & 0xFF) << 8);
            lv_obj_set_user_data(btn, (void*)(intptr_t)user_data);

            lv_obj_t* label = lv_label_create(btn);
            lv_label_set_text(label, "");
            lv_obj_center(label);

            lv_obj_add_event_cb(btn, board_square_event_cb, LV_EVENT_CLICKED, NULL);

            g_ui->board_squares[x][y] = btn;
        }
    }

    g_ui->back_btn = lv_btn_create(g_ui->screen_game);
    lv_obj_set_size(g_ui->back_btn, 50, 30);
    lv_obj_align(g_ui->back_btn, LV_ALIGN_BOTTOM_LEFT, 5, -5);
    lv_obj_add_flag(g_ui->back_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, g_ui->back_btn);
    lv_obj_t* label_back = lv_label_create(g_ui->back_btn);
    lv_label_set_text(label_back, "Back");
    lv_obj_center(label_back);

    g_ui->resign_btn = lv_btn_create(g_ui->screen_game);
    lv_obj_set_size(g_ui->resign_btn, 50, 30);
    lv_obj_align(g_ui->resign_btn, LV_ALIGN_BOTTOM_LEFT, 60, -5);
    lv_obj_add_flag(g_ui->resign_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, g_ui->resign_btn);
    lv_obj_t* label_resign = lv_label_create(g_ui->resign_btn);
    lv_label_set_text(label_resign, "Resign");
    lv_obj_center(label_resign);

    g_ui->draw_btn = lv_btn_create(g_ui->screen_game);
    lv_obj_set_size(g_ui->draw_btn, 50, 30);
    lv_obj_align(g_ui->draw_btn, LV_ALIGN_BOTTOM_LEFT, 115, -5);
    lv_obj_add_flag(g_ui->draw_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, g_ui->draw_btn);
    lv_obj_t* label_draw = lv_label_create(g_ui->draw_btn);
    lv_label_set_text(label_draw, "Draw");
    lv_obj_center(label_draw);

    lv_obj_add_flag(g_ui->resign_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ui->draw_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(g_ui->back_btn, btn_game_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(g_ui->resign_btn, btn_resign_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(g_ui->draw_btn, btn_draw_cb, LV_EVENT_CLICKED, NULL);
}

void chess_ui_init(void) {
    if (g_ui) {
        return;
    }

    g_group = lv_group_create();

    g_ui = (ChessUIContext*)calloc(1, sizeof(ChessUIContext));
    if (!g_ui) {
        ESP_LOGE(TAG, "Failed to allocate UI context");
        return;
    }

    g_game = (ChessGame*)malloc(sizeof(ChessGame));
    if (!g_game) {
        free(g_ui);
        g_ui = NULL;
        return;
    }
    chess_game_init(g_game);

    g_ui->selected_x = -1;
    g_ui->selected_y = -1;
    g_ui->has_selection = false;
    g_ui->invite_opponent_id[0] = '\0';

    create_main_menu_screen();
    create_network_setup_screen();
    create_lobby_screen();
    create_game_screen();

    g_ui->current_screen = SCREEN_MAIN_MENU;
    lv_scr_load(g_ui->screen_main);
}

lv_group_t* chess_ui_get_group(void) {
    return g_group;
}

void chess_ui_deinit(void) {
    if (g_ui) {
        if (g_game) {
            free(g_game);
        }
        free(g_ui);
        g_ui = NULL;
    }
}

void chess_ui_switch_screen(ChessScreen screen) {
    if (!g_ui) {
        return;
    }

    g_ui->current_screen = screen;

    switch (screen) {
        case SCREEN_MAIN_MENU:
            lv_scr_load(g_ui->screen_main);
            lv_obj_t* btn_local = lv_obj_get_child(g_ui->screen_main, 1);
            if (btn_local) lv_group_focus_obj(btn_local);
            break;
        case SCREEN_NETWORK_SETUP:
            lv_scr_load(g_ui->screen_network);
            if (g_ui->ssid_input) lv_group_focus_obj(g_ui->ssid_input);
            break;
        case SCREEN_LOBBY:
            lv_scr_load(g_ui->screen_lobby);
            if (g_ui->search_btn) lv_group_focus_obj(g_ui->search_btn);
            break;
        case SCREEN_GAME:
            lv_scr_load(g_ui->screen_game);
            update_board_display();
            if (network_is_connected()) {
                lv_obj_clear_flag(g_ui->resign_btn, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(g_ui->draw_btn, LV_OBJ_FLAG_HIDDEN);
            }
            if (g_ui->board_squares[0][0]) lv_group_focus_obj(g_ui->board_squares[0][0]);
            break;
    }
}

void chess_ui_update_game(const ChessGame* game) {
    if (game && g_game) {
        memcpy(g_game, game, sizeof(ChessGame));
        update_board_display();
    }
}

void chess_ui_set_network_status(const char* status) {
    if (g_ui && g_ui->network_status_label && status) {
        ui_lock_acquire();
        lv_label_set_text(g_ui->network_status_label, status);
        ui_lock_release();
    }
}

void chess_ui_show_invite_dialog(const char* opponent_id) {
    if (!g_ui || !opponent_id) {
        return;
    }

    ui_lock_acquire();

    // Store opponent ID safely
    strncpy(g_ui->invite_opponent_id, opponent_id, sizeof(g_ui->invite_opponent_id) - 1);
    g_ui->invite_opponent_id[sizeof(g_ui->invite_opponent_id) - 1] = '\0';

    // Create dialog on active screen
    g_ui->invite_dialog = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g_ui->invite_dialog, 130, 80);
    lv_obj_center(g_ui->invite_dialog);
    lv_obj_set_style_bg_color(g_ui->invite_dialog, lv_color_hex(0x2a2a4a), 0);
    lv_obj_set_style_border_width(g_ui->invite_dialog, 2, 0);
    lv_obj_set_style_border_color(g_ui->invite_dialog, lv_color_hex(0xFFFFFF), 0);

    g_ui->invite_label = lv_label_create(g_ui->invite_dialog);
    lv_label_set_text_fmt(g_ui->invite_label, "Invite from\n%s", opponent_id);
    lv_obj_set_style_text_color(g_ui->invite_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(g_ui->invite_label, LV_ALIGN_TOP_MID, 0, 10);

    g_ui->accept_btn = lv_btn_create(g_ui->invite_dialog);
    lv_obj_set_size(g_ui->accept_btn, 50, 30);
    lv_obj_align(g_ui->accept_btn, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_add_flag(g_ui->accept_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, g_ui->accept_btn);
    lv_obj_t* label_accept = lv_label_create(g_ui->accept_btn);
    lv_label_set_text(label_accept, "Accept");
    lv_obj_center(label_accept);

    g_ui->reject_btn = lv_btn_create(g_ui->invite_dialog);
    lv_obj_set_size(g_ui->reject_btn, 50, 30);
    lv_obj_align(g_ui->reject_btn, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_add_flag(g_ui->reject_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_add_obj(g_group, g_ui->reject_btn);
    lv_obj_t* label_reject = lv_label_create(g_ui->reject_btn);
    lv_label_set_text(label_reject, "Reject");
    lv_obj_center(label_reject);

    lv_obj_add_event_cb(g_ui->accept_btn, btn_accept_invite_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(g_ui->reject_btn, btn_reject_invite_cb, LV_EVENT_CLICKED, NULL);

    lv_group_focus_obj(g_ui->accept_btn);

    ui_lock_release();
}

void chess_ui_hide_invite_dialog(void) {
    if (g_ui && g_ui->invite_dialog && lv_obj_is_valid(g_ui->invite_dialog)) {
        ui_lock_acquire();
        lv_obj_del(g_ui->invite_dialog);
        g_ui->invite_dialog = NULL;
        g_ui->invite_opponent_id[0] = '\0';
        ui_lock_release();
    }
}

void chess_ui_show_message(const char* message, int duration_ms) {
    if (!g_ui || !message) {
        return;
    }

    ui_lock_acquire();
    lv_obj_t* msgbox = lv_msgbox_create(NULL);
    lv_obj_t* label = lv_label_create(msgbox);
    lv_label_set_text(label, message);
    lv_obj_center(label);
    lv_obj_set_size(msgbox, 120, 60);
    lv_obj_set_style_bg_color(msgbox, lv_color_hex(0x2a2a4a), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(msgbox);

    if (duration_ms > 0) {
        lv_timer_create(msgbox_timer_cb, duration_ms, msgbox);
    }
    ui_lock_release();
}
