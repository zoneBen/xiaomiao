#include "chess_ui.h"
#include "chess_network.h"
#include "network.h"
#include "ui_lock.h"
#include "tft_display.h"
#include "chess_pieces.h"
#include "sound.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

// Forward declarations
static void render_main_menu(void);
static void render_network_setup(void);
static void render_lobby(void);
static void render_game(void);
static void render_invite_dialog(void);
static void render_message(void);
static void chess_ui_render_internal(void);

static const char* TAG = "chess_ui";

// UI state
typedef struct {
    ChessScreen current_screen;
    int menu_index;
    int menu_count;

    // Game screen state
    int cursor_x;
    int cursor_y;
    int selected_x;
    int selected_y;
    bool has_selection;

    // Network setup screen
    char ssid[32];
    char password[32];
    char mqtt_uri[64];
    int input_field;  // 0=ssid, 1=password, 2=mqtt_uri
    int input_pos;
    int cursor_blink;

    // Lobby state
    bool searching;

    // Invite dialog
    bool show_invite;
    char invite_opponent[32];

    // Message display
    bool show_message;
    char message_text[64];
    int message_timer;

    // Sound settings
    bool music_enabled;

    // Screen buffer
    TFT_Sprite sprite;
    bool sprite_created;

    // Refresh throttling
    int64_t last_render_tick;
} ChessUIContext;

static ChessUIContext* g_ui = NULL;
static ChessGame* g_game = NULL;

// Menu items for main menu
static const char* main_menu_items[] = {
    "Local Game",
    "Network Game",
    "Music: Off"
};

// Menu items for lobby
static const char* lobby_menu_items[] = {
    "Search Opponent",
    "Stop Search",
    "Back"
};

void chess_ui_init(void)
{
    if (g_ui) {
        return;
    }

    ESP_LOGI(TAG, "Initializing chess UI");

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

    // Initialize UI state
    g_ui->current_screen = SCREEN_MAIN_MENU;
    g_ui->menu_index = 0;
    g_ui->menu_count = 3;
    g_ui->cursor_x = 0;
    g_ui->cursor_y = 0;
    g_ui->selected_x = -1;
    g_ui->selected_y = -1;
    g_ui->has_selection = false;
    g_ui->show_invite = false;
    g_ui->input_field = 0;
    g_ui->input_pos = 0;
    g_ui->searching = false;
    g_ui->music_enabled = false;

    // Default WiFi/MQTT settings
    strcpy(g_ui->mqtt_uri, "mqtt://broker.hivemq.com");

    // Create sprite buffer
    if (tft_sprite_create(&g_ui->sprite, TFT_WIDTH, TFT_HEIGHT) == ESP_OK) {
        g_ui->sprite_created = true;
        ESP_LOGI(TAG, "Sprite buffer created");
    } else {
        ESP_LOGE(TAG, "Failed to create sprite buffer");
    }

    // Initialize last render time
    g_ui->last_render_tick = esp_timer_get_time() - 100000;  // Ensure first render happens

    ESP_LOGI(TAG, "Chess UI initialized");
}

void chess_ui_deinit(void)
{
    if (g_ui) {
        if (g_ui->sprite_created) {
            tft_sprite_delete(&g_ui->sprite);
        }
        if (g_game) {
            free(g_game);
        }
        free(g_ui);
        g_ui = NULL;
    }
}

void chess_ui_switch_screen(ChessScreen screen)
{
    if (!g_ui) return;

    ESP_LOGI(TAG, "Switching to screen %d", screen);

    g_ui->current_screen = screen;
    g_ui->menu_index = 0;

    switch (screen) {
        case SCREEN_MAIN_MENU:
            g_ui->menu_count = 3;
            break;
        case SCREEN_NETWORK_SETUP:
            g_ui->menu_count = 6;
            break;
        case SCREEN_LOBBY:
            g_ui->menu_count = 3;
            break;
        case SCREEN_GAME:
            chess_game_reset(g_game);
            g_ui->cursor_x = 0;
            g_ui->cursor_y = 0;
            g_ui->selected_x = -1;
            g_ui->selected_y = -1;
            g_ui->has_selection = false;
            break;
    }
}

void chess_ui_update_game(const ChessGame* game)
{
    if (game && g_game) {
        ui_lock_acquire();
        memcpy(g_game, game, sizeof(ChessGame));
        chess_ui_render_internal();
        ui_lock_release();
    }
}

void chess_ui_set_network_status(const char* status)
{
    if (g_ui && status) {
        ESP_LOGI(TAG, "Network status: %s", status);
    }
}

void chess_ui_show_invite_dialog(const char* opponent_id)
{
    if (!g_ui || !opponent_id) return;

    ESP_LOGI(TAG, "Showing invite from %s", opponent_id);

    ui_lock_acquire();
    strncpy(g_ui->invite_opponent, opponent_id, sizeof(g_ui->invite_opponent) - 1);
    g_ui->invite_opponent[sizeof(g_ui->invite_opponent) - 1] = '\0';
    g_ui->show_invite = true;
    g_ui->menu_index = 0;
    chess_ui_render_internal();
    ui_lock_release();
}

void chess_ui_hide_invite_dialog(void)
{
    if (g_ui) {
        ui_lock_acquire();
        g_ui->show_invite = false;
        chess_ui_render_internal();
        ui_lock_release();
    }
}

void chess_ui_show_message(const char* message, int duration_ms)
{
    if (!g_ui || !message) return;

    ui_lock_acquire();
    strncpy(g_ui->message_text, message, sizeof(g_ui->message_text) - 1);
    g_ui->message_text[sizeof(g_ui->message_text) - 1] = '\0';
    g_ui->show_message = true;
    g_ui->message_timer = duration_ms / 100;  // Convert to 100ms ticks
    chess_ui_render_internal();
    ui_lock_release();
}

void chess_ui_handle_key(KeyCode key)
{
    if (!g_ui) return;

    ESP_LOGI(TAG, "Handling key %d on screen %d", key, g_ui->current_screen);

    ui_lock_acquire();

    // Handle invite dialog first
    if (g_ui->show_invite) {
        if (key == KEY_LEFT || key == KEY_RIGHT) {
            g_ui->menu_index = (g_ui->menu_index == 0) ? 1 : 0;
        } else if (key == KEY_A) {
            if (g_ui->menu_index == 0) {
                chess_network_accept_invite(g_ui->invite_opponent);
                chess_ui_switch_screen(SCREEN_GAME);
            } else {
                chess_network_reject_invite(g_ui->invite_opponent);
            }
            g_ui->show_invite = false;
        } else if (key == KEY_B) {
            g_ui->show_invite = false;
        }
        chess_ui_render_internal();
        ui_lock_release();
        return;
    }

    switch (g_ui->current_screen) {
        case SCREEN_MAIN_MENU:
            if (key == KEY_UP) {
                if (g_ui->menu_index > 0) g_ui->menu_index--;
            } else if (key == KEY_DOWN) {
                if (g_ui->menu_index < g_ui->menu_count - 1) g_ui->menu_index++;
            } else if (key == KEY_A) {
                if (g_ui->menu_index == 0) {
                    sound_play(SOUND_GAME_START);
                    chess_ui_switch_screen(SCREEN_GAME);
                } else if (g_ui->menu_index == 1) {
                    sound_play(SOUND_MENU_SELECT);
                    chess_ui_switch_screen(SCREEN_NETWORK_SETUP);
                } else if (g_ui->menu_index == 2) {
                    // Toggle music
                    g_ui->music_enabled = !g_ui->music_enabled;
                    if (g_ui->music_enabled) {
                        sound_start_background_music();
                        sound_play(SOUND_MENU_SELECT);
                    } else {
                        sound_stop_background_music();
                    }
                }
            }
            break;

        case SCREEN_NETWORK_SETUP:
            if (key == KEY_UP) {
                if (g_ui->menu_index > 0) g_ui->menu_index--;
            } else if (key == KEY_DOWN) {
                if (g_ui->menu_index < 5) g_ui->menu_index++;
            } else if (key == KEY_A) {
                if (g_ui->menu_index == 3) {
                    // Connect
                    if (strlen(g_ui->ssid) > 0) {
                        network_set_wifi_credentials(g_ui->ssid, g_ui->password);
                        network_set_mqtt_config(g_ui->mqtt_uri, chess_network_get_player_id(), "", "");
                        network_connect();
                    }
                } else if (g_ui->menu_index == 4) {
                    // Disconnect
                    network_disconnect();
                } else if (g_ui->menu_index == 5) {
                    // Back
                    if (network_is_connected()) {
                        chess_ui_switch_screen(SCREEN_LOBBY);
                    } else {
                        chess_ui_switch_screen(SCREEN_MAIN_MENU);
                    }
                }
            } else if (key == KEY_B) {
                // Back
                if (network_is_connected()) {
                    chess_ui_switch_screen(SCREEN_LOBBY);
                } else {
                    chess_ui_switch_screen(SCREEN_MAIN_MENU);
                }
            }
            break;

        case SCREEN_LOBBY:
            if (key == KEY_UP) {
                if (g_ui->menu_index > 0) g_ui->menu_index--;
            } else if (key == KEY_DOWN) {
                if (g_ui->menu_index < g_ui->menu_count - 1) g_ui->menu_index++;
            } else if (key == KEY_A) {
                if (g_ui->menu_index == 0) {
                    // Search opponent
                    if (chess_network_start_search()) {
                        g_ui->searching = true;
                    }
                } else if (g_ui->menu_index == 1) {
                    // Stop search
                    chess_network_stop_search();
                    g_ui->searching = false;
                } else {
                    // Back
                    chess_network_stop_search();
                    chess_ui_switch_screen(SCREEN_NETWORK_SETUP);
                }
            } else if (key == KEY_B) {
                chess_network_stop_search();
                chess_ui_switch_screen(SCREEN_NETWORK_SETUP);
            }
            break;

        case SCREEN_GAME:
            if (key == KEY_UP) {
                if (g_ui->cursor_y > 0) g_ui->cursor_y--;
            } else if (key == KEY_DOWN) {
                if (g_ui->cursor_y < 7) g_ui->cursor_y++;
            } else if (key == KEY_LEFT) {
                if (g_ui->cursor_x > 0) g_ui->cursor_x--;
            } else if (key == KEY_RIGHT) {
                if (g_ui->cursor_x < 7) g_ui->cursor_x++;
            } else if (key == KEY_A) {
                ChessPiece piece = chess_game_get_piece(g_game, g_ui->cursor_x, g_ui->cursor_y);

                if (g_ui->has_selection) {
                    if (g_ui->selected_x == g_ui->cursor_x && g_ui->selected_y == g_ui->cursor_y) {
                        // Deselect
                        g_ui->has_selection = false;
                        g_ui->selected_x = -1;
                        g_ui->selected_y = -1;
                    } else if (chess_game_is_valid_move(g_game, g_ui->selected_x, g_ui->selected_y,
                                                        g_ui->cursor_x, g_ui->cursor_y)) {
                        // Check if this is a capture
                        ChessPiece target_piece = chess_game_get_piece(g_game, g_ui->cursor_x, g_ui->cursor_y);
                        bool is_capture = (target_piece.type != PIECE_NONE);

                        // Make move
                        if (chess_game_make_move(g_game, g_ui->selected_x, g_ui->selected_y,
                                                g_ui->cursor_x, g_ui->cursor_y, PIECE_QUEEN)) {
                            if (is_capture) {
                                sound_play(SOUND_CAPTURE);
                            } else {
                                sound_play(SOUND_MOVE);
                            }
                            if (chess_network_get_state() == CHESS_NET_STATE_PLAYING) {
                                chess_network_send_move(g_ui->selected_x, g_ui->selected_y,
                                                        g_ui->cursor_x, g_ui->cursor_y, PIECE_QUEEN);
                            }
                        } else {
                            sound_play(SOUND_MOVE);
                        }
                        g_ui->has_selection = false;
                        g_ui->selected_x = -1;
                        g_ui->selected_y = -1;
                    } else if (piece.type != PIECE_NONE &&
                               piece.color == chess_game_get_current_turn(g_game)) {
                        // Select different piece
                        g_ui->selected_x = g_ui->cursor_x;
                        g_ui->selected_y = g_ui->cursor_y;
                    } else {
                        // Invalid move, deselect
                        sound_play(SOUND_INVALID_MOVE);
                        g_ui->has_selection = false;
                        g_ui->selected_x = -1;
                        g_ui->selected_y = -1;
                    }
                } else {
                    if (piece.type != PIECE_NONE &&
                        piece.color == chess_game_get_current_turn(g_game)) {
                        // Select piece
                        g_ui->selected_x = g_ui->cursor_x;
                        g_ui->selected_y = g_ui->cursor_y;
                        g_ui->has_selection = true;
                    } else if (piece.type != PIECE_NONE) {
                        // Tried to select opponent's piece - invalid
                        sound_play(SOUND_INVALID_MOVE);
                    } else {
                        // Tried to select empty square - invalid
                        sound_play(SOUND_INVALID_MOVE);
                    }
                }
            } else if (key == KEY_B) {
                if (g_ui->has_selection) {
                    g_ui->has_selection = false;
                    g_ui->selected_x = -1;
                    g_ui->selected_y = -1;
                } else {
                    // Back to menu
                    if (chess_network_get_state() == CHESS_NET_STATE_PLAYING) {
                        chess_network_send_resign();
                    }
                    chess_ui_switch_screen(SCREEN_MAIN_MENU);
                }
            }
            break;
    }

    chess_ui_render_internal();
    ui_lock_release();
}

void chess_ui_update(void)
{
    if (!g_ui) return;

    // Update message timer
    if (g_ui->show_message) {
        if (g_ui->message_timer > 0) {
            g_ui->message_timer--;
        } else {
            g_ui->show_message = false;
            // Don't render here - let the caller handle rendering if needed
            // to avoid lock contention
        }
    }

    // Update cursor blink
    g_ui->cursor_blink++;
}

// Internal render function - assumes lock is already held
static void chess_ui_render_internal(void)
{
    if (!g_ui || !g_ui->sprite_created) return;

    // Throttle rendering to max 30 FPS (33ms per frame)
    int64_t now = esp_timer_get_time();
    if (now - g_ui->last_render_tick < 33000) {  // 33ms = ~30 FPS
        return;
    }
    g_ui->last_render_tick = now;

    // Clear sprite
    tft_sprite_fill_screen(&g_ui->sprite, COLOR_BACKGROUND);

    // Render current screen
    switch (g_ui->current_screen) {
        case SCREEN_MAIN_MENU:
            render_main_menu();
            break;
        case SCREEN_NETWORK_SETUP:
            render_network_setup();
            break;
        case SCREEN_LOBBY:
            render_lobby();
            break;
        case SCREEN_GAME:
            render_game();
            break;
    }

    // Render invite dialog on top
    if (g_ui->show_invite) {
        render_invite_dialog();
    }

    // Render message
    if (g_ui->show_message) {
        render_message();
    }

    // Push sprite to display
    tft_sprite_push(&g_ui->sprite, 0, 0);
}

void chess_ui_render(void)
{
    if (!g_ui || !g_ui->sprite_created) return;

    ui_lock_acquire();
    chess_ui_render_internal();
    ui_lock_release();
}

static void render_main_menu(void)
{
    // Title
    int title_x = (TFT_WIDTH - tft_sprite_text_width("CHESS", 2)) / 2;
    for (int i = 0; i < 5; i++) {
        char c = "CHESS"[i];
        tft_sprite_draw_char(&g_ui->sprite, title_x + i * 12, 4, c, TFT_WHITE, 2);
    }

    // Menu items
    for (int i = 0; i < 3; i++) {
        int y = 40 + i * 25;
        bool is_selected = (i == g_ui->menu_index);

        if (is_selected) {
            tft_sprite_fill_rect(&g_ui->sprite, 20, y, 120, 20, TFT_NAVY);
        }

        int text_x = 25;
        const char* text = main_menu_items[i];
        if (i == 2) {
            // Dynamic music option text
            text = g_ui->music_enabled ? "Music: On " : "Music: Off";
        }
        while (*text) {
            tft_sprite_draw_char(&g_ui->sprite, text_x, y + 6, *text, is_selected ? TFT_YELLOW : TFT_WHITE, 1);
            text_x += 6;
            text++;
        }
    }

    // Player ID
    char id_text[32];
    snprintf(id_text, sizeof(id_text), "ID: %s", chess_network_get_player_id());
    int id_x = (TFT_WIDTH - tft_sprite_text_width(id_text, 1)) / 2;
    int text_x = id_x;
    const char* text = id_text;
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 110, *text, TFT_LIGHTGREY, 1);
        text_x += 6;
        text++;
    }
}

static void render_network_setup(void)
{
    // Title
    int title_x = (TFT_WIDTH - tft_sprite_text_width("Network Setup", 1)) / 2;
    int text_x = title_x;
    const char* text = "Network Setup";
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 5, *text, TFT_WHITE, 1);
        text_x += 6;
        text++;
    }

    // Menu items
    const char* items[] = {"WiFi SSID:", "Password:", "MQTT Broker:", "Connect", "Disconnect", "Back"};
    for (int i = 0; i < 6; i++) {
        int y = 20 + i * 15;
        bool is_selected = (i == g_ui->menu_index);

        if (is_selected) {
            tft_sprite_fill_rect(&g_ui->sprite, 5, y, 150, 14, TFT_NAVY);
        }

        // Label
        text_x = 10;
        text = items[i];
        while (*text) {
            tft_sprite_draw_char(&g_ui->sprite, text_x, y + 4, *text, is_selected ? TFT_YELLOW : TFT_WHITE, 1);
            text_x += 6;
            text++;
        }

        // Values for input fields
        if (i == 0 && strlen(g_ui->ssid) > 0) {
            text = g_ui->ssid;
            text_x = 70;
            while (*text && text_x < 155) {
                tft_sprite_draw_char(&g_ui->sprite, text_x, y + 4, *text, is_selected ? TFT_YELLOW : TFT_LIGHTGREY, 1);
                text_x += 6;
                text++;
            }
        } else if (i == 1 && strlen(g_ui->password) > 0) {
            text = "********";
            text_x = 70;
            while (*text && text_x < 155) {
                tft_sprite_draw_char(&g_ui->sprite, text_x, y + 4, *text, is_selected ? TFT_YELLOW : TFT_LIGHTGREY, 1);
                text_x += 6;
                text++;
            }
        } else if (i == 2) {
            text = g_ui->mqtt_uri;
            text_x = 70;
            while (*text && text_x < 155) {
                tft_sprite_draw_char(&g_ui->sprite, text_x, y + 4, *text, is_selected ? TFT_YELLOW : TFT_LIGHTGREY, 1);
                text_x += 6;
                text++;
            }
        }
    }

    // Status
    const char* status_text = network_is_connected() ? "Status: Connected" : "Status: Disconnected";
    uint16_t status_color = network_is_connected() ? TFT_GREEN : TFT_RED;
    text_x = 10;
    text = status_text;
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 118, *text, status_color, 1);
        text_x += 6;
        text++;
    }
}

static void render_lobby(void)
{
    // Title
    int title_x = (TFT_WIDTH - tft_sprite_text_width("Game Lobby", 1)) / 2;
    int text_x = title_x;
    const char* text = "Game Lobby";
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 5, *text, TFT_WHITE, 1);
        text_x += 6;
        text++;
    }

    // Status
    const char* status_text = g_ui->searching ? "Searching for opponent..." : "Status: Waiting";
    text_x = 10;
    text = status_text;
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 20, *text, g_ui->searching ? TFT_YELLOW : TFT_LIGHTGREY, 1);
        text_x += 6;
        text++;
    }

    // Menu items
    int display_i = 0;
    for (int i = 0; i < 3; i++) {
        if (i == 1 && !g_ui->searching) continue;

        int y = 40 + display_i * 25;
        bool is_selected = (i == g_ui->menu_index);

        if (is_selected) {
            tft_sprite_fill_rect(&g_ui->sprite, 20, y, 120, 20, TFT_NAVY);
        }

        text_x = 25;
        text = lobby_menu_items[i];
        while (*text) {
            tft_sprite_draw_char(&g_ui->sprite, text_x, y + 6, *text, is_selected ? TFT_YELLOW : TFT_WHITE, 1);
            text_x += 6;
            text++;
        }
        display_i++;
    }
}

static void render_game(void)
{
    // Chess board (128x128, centered or left-aligned)
    int board_offset_x = 0;  // Left align to leave room for sidebar
    int board_offset_y = 0;
    int square_size = SQUARE_SIZE;

    // Draw the board
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            ChessPiece piece = chess_game_get_piece(g_game, x, y);
            bool is_cursor = (x == g_ui->cursor_x && y == g_ui->cursor_y);
            bool is_selected = g_ui->has_selection && (x == g_ui->selected_x && y == g_ui->selected_y);

            // Draw square
            int px = board_offset_x + x * square_size;
            int py = board_offset_y + y * square_size;
            uint16_t bg_color = ((x + y) % 2 == 0) ? COLOR_LIGHT_SQUARE : COLOR_DARK_SQUARE;

            tft_sprite_fill_rect(&g_ui->sprite, px, py, square_size, square_size, bg_color);

            // Draw piece if present
            if (piece.type != PIECE_NONE) {
                // Get the piece bitmap
                int piece_idx = piece.type - 1;  // 0=pawn, 1=knight, etc.
                const uint8_t* bitmap = piece_images[piece.color][piece_idx];

                // Center the 14x14 piece in the 16x16 square
                int piece_x = px + 1;
                int piece_y = py + 1;
                uint16_t piece_color = (piece.color == COLOR_WHITE) ? COLOR_WHITE_PIECE : COLOR_BLACK_PIECE;

                // Draw the piece bitmap
                for (int yy = 0; yy < PIECE_HEIGHT; yy++) {
                    for (int xx = 0; xx < PIECE_WIDTH; xx++) {
                        int byte_idx = yy * 2 + (xx / 8);
                        int bit_idx = 7 - (xx % 8);
                        if (bitmap[byte_idx] & (1 << bit_idx)) {
                            tft_sprite_draw_pixel(&g_ui->sprite, piece_x + xx, piece_y + yy, piece_color);
                        }
                    }
                }
            }

            // Draw cursor border
            if (is_cursor) {
                tft_sprite_fill_rect(&g_ui->sprite, px, py, square_size, 1, COLOR_CURSOR);
                tft_sprite_fill_rect(&g_ui->sprite, px, py + square_size - 1, square_size, 1, COLOR_CURSOR);
                tft_sprite_fill_rect(&g_ui->sprite, px, py, 1, square_size, COLOR_CURSOR);
                tft_sprite_fill_rect(&g_ui->sprite, px + square_size - 1, py, 1, square_size, COLOR_CURSOR);
            }

            // Draw selected highlight
            if (is_selected) {
                tft_sprite_fill_rect(&g_ui->sprite, px + 2, py + 2, square_size - 4, square_size - 4, COLOR_SELECTED);
                // Redraw piece on top of highlight
                if (piece.type != PIECE_NONE) {
                    int piece_idx = piece.type - 1;
                    const uint8_t* bitmap = piece_images[piece.color][piece_idx];
                    int piece_x = px + 1;
                    int piece_y = py + 1;
                    uint16_t piece_color = (piece.color == COLOR_WHITE) ? COLOR_WHITE_PIECE : COLOR_BLACK_PIECE;

                    for (int yy = 0; yy < PIECE_HEIGHT; yy++) {
                        for (int xx = 0; xx < PIECE_WIDTH; xx++) {
                            int byte_idx = yy * 2 + (xx / 8);
                            int bit_idx = 7 - (xx % 8);
                            if (bitmap[byte_idx] & (1 << bit_idx)) {
                                tft_sprite_draw_pixel(&g_ui->sprite, piece_x + xx, piece_y + yy, piece_color);
                            }
                        }
                    }
                }
            }
        }
    }

    // Sidebar (right side, 32x128)
    int sidebar_x = 128;

    // Turn indicator
    ChessColor turn = chess_game_get_current_turn(g_game);
    tft_sprite_fill_rect(&g_ui->sprite, sidebar_x, 0, 32, 32, COLOR_BACKGROUND);
    tft_sprite_fill_rect(&g_ui->sprite, sidebar_x + 8, 8, 16, 16, (turn == COLOR_WHITE) ? TFT_WHITE : TFT_BLACK);
    tft_sprite_draw_rect(&g_ui->sprite, sidebar_x + 8, 8, 16, 16, TFT_LIGHTGREY);

    // Captured pieces area (simplified)
    tft_sprite_fill_rect(&g_ui->sprite, sidebar_x, 32, 32, 64, COLOR_BACKGROUND);

    // Move count
    char move_text[16];
    snprintf(move_text, sizeof(move_text), "M:%d", g_game->fullmove_number);
    int text_x = sidebar_x + 4;
    const char* text = move_text;
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 100, *text, TFT_LIGHTGREY, 1);
        text_x += 6;
        text++;
    }
}

static void render_invite_dialog(void)
{
    // Dialog background
    tft_sprite_fill_rect(&g_ui->sprite, 15, 35, 130, 60, COLOR_DIALOG);
    tft_sprite_draw_rect(&g_ui->sprite, 15, 35, 130, 60, TFT_WHITE);

    // Title
    int title_x = (TFT_WIDTH - tft_sprite_text_width("Invite from:", 1)) / 2;
    int text_x = title_x;
    const char* text = "Invite from:";
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 45, *text, TFT_WHITE, 1);
        text_x += 6;
        text++;
    }

    // Opponent ID
    int id_x = (TFT_WIDTH - tft_sprite_text_width(g_ui->invite_opponent, 1)) / 2;
    text_x = id_x;
    text = g_ui->invite_opponent;
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 58, *text, TFT_YELLOW, 1);
        text_x += 6;
        text++;
    }

    // Buttons
    const char* btn1 = "Accept";
    const char* btn2 = "Reject";

    // Button 1
    bool is_selected = (g_ui->menu_index == 0);
    if (is_selected) {
        tft_sprite_fill_rect(&g_ui->sprite, 25, 75, 50, 14, TFT_NAVY);
    }
    text_x = 30;
    text = btn1;
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 80, *text, is_selected ? TFT_YELLOW : TFT_WHITE, 1);
        text_x += 6;
        text++;
    }

    // Button 2
    is_selected = (g_ui->menu_index == 1);
    if (is_selected) {
        tft_sprite_fill_rect(&g_ui->sprite, 85, 75, 50, 14, TFT_NAVY);
    }
    text_x = 90;
    text = btn2;
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 80, *text, is_selected ? TFT_YELLOW : TFT_WHITE, 1);
        text_x += 6;
        text++;
    }
}

static void render_message(void)
{
    // Simple message box
    int msg_width = tft_sprite_text_width(g_ui->message_text, 1) + 10;
    int msg_x = (TFT_WIDTH - msg_width) / 2;
    int msg_y = 50;

    tft_sprite_fill_rect(&g_ui->sprite, msg_x - 5, msg_y - 5, msg_width + 10, 22, COLOR_DIALOG);
    tft_sprite_draw_rect(&g_ui->sprite, msg_x - 5, msg_y - 5, msg_width + 10, 22, TFT_WHITE);

    int text_x = msg_x;
    const char* text = g_ui->message_text;
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, msg_y, *text, TFT_WHITE, 1);
        text_x += 6;
        text++;
    }
}
