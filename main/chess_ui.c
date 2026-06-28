#include "chess_ui.h"
#include "chess_bluetooth.h"
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
static void render_bluetooth_menu(void);
static void render_bluetooth_scan(void);
static void render_bluetooth_waiting(void);
static void render_game(void);
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
    int cursor_blink;

    // Bluetooth state
    bool bluetooth_initialized;
    int selected_device_index;
    char bluetooth_status[64];

    // Message display
    bool show_message;
    char message_text[64];
    int message_timer;

    // Sound settings
    bool sound_enabled;

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
    "Bluetooth Game",
    "Sound: Off"
};

// Menu items for Bluetooth menu
static const char* bluetooth_menu_items[] = {
    "Create Game",
    "Join Game",
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
    g_ui->bluetooth_initialized = false;
    g_ui->selected_device_index = -1;
    strcpy(g_ui->bluetooth_status, "Bluetooth: Disabled");
    g_ui->sound_enabled = true;
    g_ui->cursor_blink = 0;
    sound_set_effects_enabled(true);

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
        case SCREEN_BLUETOOTH_MENU:
            g_ui->menu_count = 3;
            // Initialize Bluetooth if not already
            if (!g_ui->bluetooth_initialized) {
                bt_chess_init();
                g_ui->bluetooth_initialized = true;
                strcpy(g_ui->bluetooth_status, "Bluetooth: Ready");
            }
            break;
        case SCREEN_BLUETOOTH_SCAN:
            g_ui->menu_count = 0;
            g_ui->selected_device_index = -1;
            break;
        case SCREEN_BLUETOOTH_WAITING:
            g_ui->menu_count = 0;
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

void chess_ui_apply_opponent_move(int from_x, int from_y, int to_x, int to_y, ChessPieceType promotion)
{
    if (!g_ui || !g_game) return;

    ui_lock_acquire();

    // Apply the move
    if (chess_game_make_move(g_game, from_x, from_y, to_x, to_y, promotion)) {
        // Play appropriate sound
        ChessPiece captured = g_game->last_move.captured_piece;
        if (captured.type != PIECE_NONE || g_game->last_move.is_en_passant) {
            sound_play(SOUND_CAPTURE);
        } else {
            sound_play(SOUND_MOVE);
        }

        // Check for game over
        if (chess_game_is_game_over(g_game)) {
            sound_play(SOUND_CHECKMATE);
        }
    }

    chess_ui_render_internal();
    ui_lock_release();
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


// Internal version - assumes lock is already held
static void chess_ui_show_message_internal(const char* message, int duration_ms)
{
    if (!g_ui || !message) return;

    strncpy(g_ui->message_text, message, sizeof(g_ui->message_text) - 1);
    g_ui->message_text[sizeof(g_ui->message_text) - 1] = '\0';
    g_ui->show_message = true;
    g_ui->message_timer = duration_ms / 100;  // Convert to 100ms ticks
    chess_ui_render_internal();
}

void chess_ui_show_message(const char* message, int duration_ms)
{
    if (!g_ui || !message) return;

    ui_lock_acquire();
    chess_ui_show_message_internal(message, duration_ms);
    ui_lock_release();
}

void chess_ui_handle_key(KeyCode key)
{
    if (!g_ui) return;

    ESP_LOGI(TAG, "Handling key %d on screen %d", key, g_ui->current_screen);

    ui_lock_acquire();

    switch (g_ui->current_screen) {
        case SCREEN_MAIN_MENU:
            if (key == KEY_UP) {
                if (g_ui->menu_index > 0) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->menu_index--;
                }
            } else if (key == KEY_DOWN) {
                if (g_ui->menu_index < g_ui->menu_count - 1) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->menu_index++;
                }
            } else if (key == KEY_A) {
                if (g_ui->menu_index == 0) {
                    sound_play(SOUND_GAME_START);
                    chess_ui_switch_screen(SCREEN_GAME);
                } else if (g_ui->menu_index == 1) {
                    sound_play(SOUND_MENU_SELECT);
                    chess_ui_switch_screen(SCREEN_BLUETOOTH_MENU);
                } else if (g_ui->menu_index == 2) {
                    // Toggle sound effects
                    g_ui->sound_enabled = !g_ui->sound_enabled;
                    sound_set_effects_enabled(g_ui->sound_enabled);
                    if (g_ui->sound_enabled) {
                        sound_play(SOUND_MENU_SELECT);
                    }
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
            }
            break;

        case SCREEN_BLUETOOTH_MENU:
            if (key == KEY_UP) {
                if (g_ui->menu_index > 0) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->menu_index--;
                }
            } else if (key == KEY_DOWN) {
                if (g_ui->menu_index < 2) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->menu_index++;
                }
            } else if (key == KEY_A) {
                sound_play(SOUND_MENU_SELECT);
                if (g_ui->menu_index == 0) {
                    // Create game (host)
                    if (bt_chess_start_hosting()) {
                        strcpy(g_ui->bluetooth_status, "Waiting for opponent...");
                        chess_ui_switch_screen(SCREEN_BLUETOOTH_WAITING);
                    }
                } else if (g_ui->menu_index == 1) {
                    // Join game (scan)
                    if (bt_chess_start_scanning()) {
                        strcpy(g_ui->bluetooth_status, "Scanning...");
                        chess_ui_switch_screen(SCREEN_BLUETOOTH_SCAN);
                    }
                } else {
                    // Back
                    chess_ui_switch_screen(SCREEN_MAIN_MENU);
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                chess_ui_switch_screen(SCREEN_MAIN_MENU);
            }
            break;

        case SCREEN_BLUETOOTH_SCAN:
            if (key == KEY_UP) {
                if (g_ui->selected_device_index > 0) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->selected_device_index--;
                }
            } else if (key == KEY_DOWN) {
                int device_count = bt_chess_get_found_device_count();
                if (g_ui->selected_device_index < device_count - 1) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->selected_device_index++;
                }
            } else if (key == KEY_A) {
                if (g_ui->selected_device_index >= 0) {
                    sound_play(SOUND_MENU_SELECT);
                    if (bt_chess_connect_to_device(g_ui->selected_device_index)) {
                        strcpy(g_ui->bluetooth_status, "Connecting...");
                        chess_ui_switch_screen(SCREEN_BLUETOOTH_WAITING);
                    }
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                bt_chess_stop_scanning();
                chess_ui_switch_screen(SCREEN_BLUETOOTH_MENU);
            }
            break;

        case SCREEN_BLUETOOTH_WAITING:
            if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                bt_chess_disconnect();
                bt_chess_stop_hosting();
                bt_chess_stop_scanning();
                chess_ui_switch_screen(SCREEN_BLUETOOTH_MENU);
            }
            break;

        case SCREEN_GAME:
            // If game is over, only handle B key to go back to menu
            if (chess_game_is_game_over(g_game)) {
                if (key == KEY_B) {
                    sound_play(SOUND_KEY_CLICK);
                    chess_ui_switch_screen(SCREEN_MAIN_MENU);
                } else if (key == KEY_A) {
                    // Optionally, allow A key to also go back to menu for convenience
                    sound_play(SOUND_KEY_CLICK);
                    chess_ui_switch_screen(SCREEN_MAIN_MENU);
                }
                break;
            }

            // Check if it's our turn in bluetooth game
            bool is_bluetooth_game = (bt_chess_get_state() == BT_CHESS_STATE_GAME_READY ||
                                     bt_chess_get_state() == BT_CHESS_STATE_PLAYING);
            bool is_our_turn = true;
            if (is_bluetooth_game) {
                is_our_turn = bt_chess_is_my_turn();
            }

            // Normal game handling when game is not over
            if (key == KEY_UP) {
                if (g_ui->cursor_y > 0) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->cursor_y--;
                }
            } else if (key == KEY_DOWN) {
                if (g_ui->cursor_y < 7) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->cursor_y++;
                }
            } else if (key == KEY_LEFT) {
                if (g_ui->cursor_x > 0) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->cursor_x--;
                }
            } else if (key == KEY_RIGHT) {
                if (g_ui->cursor_x < 7) {
                    sound_play(SOUND_KEY_CLICK);
                    g_ui->cursor_x++;
                }
            } else if (key == KEY_A) {
                ChessPiece piece = chess_game_get_piece(g_game, g_ui->cursor_x, g_ui->cursor_y);

                // Only allow moves if it's our turn
                if (!is_our_turn && is_bluetooth_game) {
                    sound_play(SOUND_INVALID_MOVE);
                    chess_ui_show_message_internal("Not your turn!", 1000);
                    break;
                }

                if (g_ui->has_selection) {
                    if (g_ui->selected_x == g_ui->cursor_x && g_ui->selected_y == g_ui->cursor_y) {
                        // Deselect
                        sound_play(SOUND_KEY_CLICK);
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

                            // Check for game over
                            if (chess_game_is_game_over(g_game)) {
                                ChessColor winner = chess_game_get_winner(g_game);
                                if (winner == COLOR_WHITE) {
                                    chess_ui_show_message_internal("White captured king!", 3000);
                                    sound_play(SOUND_CHECKMATE);
                                } else if (winner == COLOR_BLACK) {
                                    chess_ui_show_message_internal("Black captured king!", 3000);
                                    sound_play(SOUND_CHECKMATE);
                                }
                                // Send game over
                                if (is_bluetooth_game) {
                                    bt_chess_send_game_over(winner);
                                }
                            }

                            if (is_bluetooth_game) {
                                bt_chess_send_move(g_ui->selected_x, g_ui->selected_y,
                                                   g_ui->cursor_x, g_ui->cursor_y, PIECE_QUEEN);
                            }
                        } else {
                            sound_play(SOUND_INVALID_MOVE);
                        }
                        g_ui->has_selection = false;
                        g_ui->selected_x = -1;
                        g_ui->selected_y = -1;
                    } else if (piece.type != PIECE_NONE &&
                               piece.color == chess_game_get_current_turn(g_game)) {
                        // Select different piece
                        sound_play(SOUND_MENU_SELECT);
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
                        sound_play(SOUND_MENU_SELECT);
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
                sound_play(SOUND_KEY_CLICK);
                if (g_ui->has_selection) {
                    g_ui->has_selection = false;
                    g_ui->selected_x = -1;
                    g_ui->selected_y = -1;
                } else {
                    // Back to menu - send resignation if in game
                    if (is_bluetooth_game) {
                        bt_chess_send_resign();
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
        case SCREEN_BLUETOOTH_MENU:
            render_bluetooth_menu();
            break;
        case SCREEN_BLUETOOTH_SCAN:
            render_bluetooth_scan();
            break;
        case SCREEN_BLUETOOTH_WAITING:
            render_bluetooth_waiting();
            break;
        case SCREEN_GAME:
            render_game();
            break;
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
            // Dynamic sound option text
            text = g_ui->sound_enabled ? "Sound: On " : "Sound: Off";
        }
        while (*text) {
            tft_sprite_draw_char(&g_ui->sprite, text_x, y + 6, *text, is_selected ? TFT_YELLOW : TFT_WHITE, 1);
            text_x += 6;
            text++;
        }
    }
}



static void render_bluetooth_menu(void)
{
    // Title
    int title_x = (TFT_WIDTH - tft_sprite_text_width("Bluetooth Game", 1)) / 2;
    int text_x = title_x;
    const char* text = "Bluetooth Game";
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 5, *text, TFT_WHITE, 1);
        text_x += 6;
        text++;
    }

    // Status
    text_x = 10;
    text = g_ui->bluetooth_status;
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 20, *text, TFT_LIGHTGREY, 1);
        text_x += 6;
        text++;
    }

    // Menu items
    for (int i = 0; i < 3; i++) {
        int y = 40 + i * 25;
        bool is_selected = (i == g_ui->menu_index);

        if (is_selected) {
            tft_sprite_fill_rect(&g_ui->sprite, 20, y, 120, 20, TFT_NAVY);
        }

        text_x = 25;
        text = bluetooth_menu_items[i];
        while (*text) {
            tft_sprite_draw_char(&g_ui->sprite, text_x, y + 6, *text, is_selected ? TFT_YELLOW : TFT_WHITE, 1);
            text_x += 6;
            text++;
        }
    }
}

static void render_bluetooth_scan(void)
{
    // Title
    int title_x = (TFT_WIDTH - tft_sprite_text_width("Scan Results", 1)) / 2;
    int text_x = title_x;
    const char* text = "Scan Results";
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 5, *text, TFT_WHITE, 1);
        text_x += 6;
        text++;
    }

    // Found devices
    int device_count = bt_chess_get_found_device_count();
    int max_display = 4;  // Display up to 4 devices
    int display_count = (device_count < max_display) ? device_count : max_display;

    if (device_count == 0) {
        text_x = 10;
        text = "Searching...";
        while (*text) {
            tft_sprite_draw_char(&g_ui->sprite, text_x, 25, *text, TFT_YELLOW, 1);
            text_x += 6;
            text++;
        }
    } else {
        for (int i = 0; i < display_count; i++) {
            const FoundBluetoothDevice* device = bt_chess_get_found_device(i);
            if (!device) continue;

            int y = 25 + i * 25;
            bool is_selected = (i == g_ui->selected_device_index);

            if (is_selected) {
                tft_sprite_fill_rect(&g_ui->sprite, 5, y, TFT_WIDTH - 10, 20, TFT_NAVY);
            }

            // Display name/hash
            text_x = 10;
            char display_name[32];
            snprintf(display_name, sizeof(display_name), "%s", device->name);
            text = display_name;
            while (*text && text_x < TFT_WIDTH - 10) {
                tft_sprite_draw_char(&g_ui->sprite, text_x, y + 6, *text, is_selected ? TFT_YELLOW : TFT_WHITE, 1);
                text_x += 6;
                text++;
            }
        }
    }

    // Instructions
    text_x = 10;
    text = "B: Back";
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, TFT_HEIGHT - 12, *text, TFT_LIGHTGREY, 1);
        text_x += 6;
        text++;
    }
}

static void render_bluetooth_waiting(void)
{
    // Title
    int title_x = (TFT_WIDTH - tft_sprite_text_width("Waiting...", 1)) / 2;
    int text_x = title_x;
    const char* text = "Waiting...";
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 40, *text, TFT_WHITE, 1);
        text_x += 6;
        text++;
    }

    // Status
    text_x = 10;
    text = g_ui->bluetooth_status;
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 60, *text, TFT_YELLOW, 1);
        text_x += 6;
        text++;
    }

    // Instructions
    text_x = 10;
    text = "B: Cancel";
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, TFT_HEIGHT - 12, *text, TFT_LIGHTGREY, 1);
        text_x += 6;
        text++;
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

    // Check indicator
    if (chess_game_is_in_check(g_game, turn)) {
        int check_x = sidebar_x + 8;
        int check_y = 24;
        const char* check_text = "+";
        while (*check_text) {
            tft_sprite_draw_char(&g_ui->sprite, check_x, check_y, *check_text, TFT_RED, 1);
            check_x += 6;
            check_text++;
        }
    }

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

    // Bluetooth game indicator
    BluetoothChessState bt_state = bt_chess_get_state();
    if (bt_state >= BT_CHESS_STATE_GAME_READY) {
        // Show if it's our turn
        uint16_t turn_color = bt_chess_is_my_turn() ? TFT_GREEN : TFT_RED;
        tft_sprite_fill_rect(&g_ui->sprite, sidebar_x, 110, 32, 14, turn_color);
        const char* turn_text = bt_chess_is_my_turn() ? "YOU" : "OPP";
        text_x = sidebar_x + 6;
        int i = 0;
        while (turn_text[i]) {
            tft_sprite_draw_char(&g_ui->sprite, text_x, 114, turn_text[i], TFT_BLACK, 1);
            text_x += 6;
            i++;
        }
    }

    // Game over overlay
    if (chess_game_is_game_over(g_game)) {
        // Dim background
        for (int x = 0; x < TFT_WIDTH; x++) {
            for (int y = 0; y < TFT_HEIGHT; y++) {
                // Simple dimming by drawing semi-transparent overlay
                // Since we don't have alpha, just draw a dark box
            }
        }

        // Draw game over dialog
        tft_sprite_fill_rect(&g_ui->sprite, 10, 40, 140, 50, COLOR_DIALOG);
        tft_sprite_draw_rect(&g_ui->sprite, 10, 40, 140, 50, TFT_WHITE);

        // Game over text
        ChessColor winner = chess_game_get_winner(g_game);
        const char* title_text = (winner == COLOR_WHITE) ? "White Captured King!" :
                                 (winner == COLOR_BLACK) ? "Black Captured King!" : "";
        int title_x = (TFT_WIDTH - tft_sprite_text_width(title_text, 1)) / 2;
        text_x = title_x;
        text = title_text;
        while (*text) {
            tft_sprite_draw_char(&g_ui->sprite, text_x, 55, *text, TFT_WHITE, 1);
            text_x += 6;
            text++;
        }

        // Press B to continue
        const char* hint_text = "B: Menu";
        int hint_x = (TFT_WIDTH - tft_sprite_text_width(hint_text, 1)) / 2;
        text_x = hint_x;
        text = hint_text;
        while (*text) {
            tft_sprite_draw_char(&g_ui->sprite, text_x, 75, *text, TFT_LIGHTGREY, 1);
            text_x += 6;
            text++;
        }
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
