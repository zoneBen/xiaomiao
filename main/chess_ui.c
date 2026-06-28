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
static void render_exit_confirm(void);
static void render_message(void);

static const char* TAG = "chess_ui";

// UI state
typedef struct {
    ChessScreen current_screen;
    ChessScreen previous_screen; // For exit confirm
    int menu_index;
    int menu_count;

    // Game screen state
    int cursor_x;
    int cursor_y;
    int selected_x;
    int selected_y;
    bool has_selection;
    int cursor_blink;

    // Last move highlighting
    int last_move_from_x;
    int last_move_from_y;
    int last_move_to_x;
    int last_move_to_y;
    bool has_last_move;

    // Scores based on piece values: P=1, N=3, B=3, R=5, Q=9
    int white_material;
    int black_material;

    // Separate timers for each player
    int64_t white_elapsed_us;  // White's total time in microseconds
    int64_t black_elapsed_us;  // Black's total time in microseconds
    int64_t turn_start_time;   // When current turn started
    ChessColor current_turn_tracked; // Which turn we're tracking

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
    int64_t last_update_time;
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

// Exit confirm menu items
static const char* exit_confirm_items[] = {
    "Confirm Exit",
    "Cancel"
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
    g_ui->previous_screen = SCREEN_MAIN_MENU;
    g_ui->menu_index = 0;
    g_ui->menu_count = 3;
    g_ui->cursor_x = 0;
    g_ui->cursor_y = 0;
    g_ui->selected_x = -1;
    g_ui->selected_y = -1;
    g_ui->has_selection = false;

    // Initialize last move tracking
    g_ui->last_move_from_x = -1;
    g_ui->last_move_from_y = -1;
    g_ui->last_move_to_x = -1;
    g_ui->last_move_to_y = -1;
    g_ui->has_last_move = false;

    // Initialize scores and timers
    g_ui->white_material = 0;
    g_ui->black_material = 0;
    g_ui->white_elapsed_us = 0;
    g_ui->black_elapsed_us = 0;
    g_ui->turn_start_time = 0;
    g_ui->current_turn_tracked = COLOR_WHITE;

    g_ui->bluetooth_initialized = false;
    g_ui->selected_device_index = -1;
    strcpy(g_ui->bluetooth_status, "Bluetooth: Disabled");
    g_ui->sound_enabled = true;
    g_ui->cursor_blink = 0;
    sound_set_effects_enabled(true);
    g_ui->last_update_time = esp_timer_get_time();

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

// Helper: Get piece value in material points
static int get_piece_value(ChessPieceType type) {
    switch(type) {
        case PIECE_PAWN: return 1;
        case PIECE_KNIGHT: return 3;
        case PIECE_BISHOP: return 3;
        case PIECE_ROOK: return 5;
        case PIECE_QUEEN: return 9;
        default: return 0;
    }
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

    // Clean up Bluetooth when returning to main menu
    if (screen == SCREEN_MAIN_MENU &&
        (g_ui->current_screen == SCREEN_BLUETOOTH_SCAN ||
         g_ui->current_screen == SCREEN_BLUETOOTH_WAITING ||
         g_ui->current_screen == SCREEN_BLUETOOTH_MENU)) {
        ESP_LOGI(TAG, "Cleaning up Bluetooth on return to main menu");
        bt_chess_stop_scanning();
        bt_chess_stop_hosting();
        // Don't fully disconnect/deinit as we might use it again soon
    }

    g_ui->previous_screen = g_ui->current_screen;
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
            // 只有当不是从重连状态回来时才重置棋盘
            BluetoothChessState bt_state = bt_chess_get_state();
            bool was_reconnecting = (bt_state == BT_CHESS_STATE_GAME_READY ||
                                     bt_state == BT_CHESS_STATE_PLAYING ||
                                     bt_state == BT_CHESS_STATE_RECONNECTING);

            if (!was_reconnecting) {
                // 新游戏才重置
                chess_game_reset(g_game);
                g_ui->cursor_x = 0;
                g_ui->cursor_y = 0;
                g_ui->selected_x = -1;
                g_ui->selected_y = -1;
                g_ui->has_selection = false;

                // Reset last move, scores and timers
                g_ui->last_move_from_x = -1;
                g_ui->last_move_from_y = -1;
                g_ui->last_move_to_x = -1;
                g_ui->last_move_to_y = -1;
                g_ui->has_last_move = false;
                g_ui->white_material = 0;
                g_ui->black_material = 0;
                g_ui->white_elapsed_us = 0;
                g_ui->black_elapsed_us = 0;
                g_ui->current_turn_tracked = COLOR_WHITE;
                g_ui->turn_start_time = esp_timer_get_time();
            }
            break;
        case SCREEN_EXIT_CONFIRM:
            g_ui->menu_count = 2;
            break;
    }
}

// Internal version - assumes lock is already held
void chess_ui_apply_opponent_move_locked(int from_x, int from_y, int to_x, int to_y, ChessPieceType promotion)
{
    if (!g_ui || !g_game) return;

    // Get the turn before making move (this is who is making the move)
    ChessColor moving_color = chess_game_get_current_turn(g_game);

    // Update timer for current player before switching turn
    int64_t now = esp_timer_get_time();
    if (moving_color == COLOR_WHITE) {
        g_ui->white_elapsed_us += now - g_ui->turn_start_time;
    } else {
        g_ui->black_elapsed_us += now - g_ui->turn_start_time;
    }

    // Apply the move
    if (chess_game_make_move(g_game, from_x, from_y, to_x, to_y, promotion)) {
        // 保存游戏状态用于同步
        if (bt_chess_get_state() >= BT_CHESS_STATE_GAME_READY) {
            bt_chess_save_game_state(g_game);
        }
        // Track the last move for highlighting
        g_ui->last_move_from_x = from_x;
        g_ui->last_move_from_y = from_y;
        g_ui->last_move_to_x = to_x;
        g_ui->last_move_to_y = to_y;
        g_ui->has_last_move = true;

        // Check for capture and update material count
        ChessPiece captured = g_game->last_move.captured_piece;
        if (captured.type != PIECE_NONE || g_game->last_move.is_en_passant) {
            sound_play(SOUND_CAPTURE);
            // Add material value to the capturer (moving_color captured the piece)
            int value = get_piece_value(captured.type);
            if (value > 0) {
                if (moving_color == COLOR_WHITE) {
                    g_ui->white_material += value;
                } else {
                    g_ui->black_material += value;
                }
            }
        } else {
            sound_play(SOUND_MOVE);
        }

        // Check for game over
        if (chess_game_is_game_over(g_game)) {
            sound_play(SOUND_CHECKMATE);
        }
    }

    // Start timer for new turn
    g_ui->turn_start_time = esp_timer_get_time();
    g_ui->current_turn_tracked = chess_game_get_current_turn(g_game);

    chess_ui_render_locked();
}

void chess_ui_apply_opponent_move(int from_x, int from_y, int to_x, int to_y, ChessPieceType promotion)
{
    if (!g_ui || !g_game) return;

    ui_lock_acquire();
    chess_ui_apply_opponent_move_locked(from_x, from_y, to_x, to_y, promotion);
    ui_lock_release();
}

// Internal version - assumes lock is already held
void chess_ui_update_game_locked(const ChessGame* game)
{
    if (game && g_game) {
        memcpy(g_game, game, sizeof(ChessGame));

        // 更新 UI 的回合跟踪
        g_ui->current_turn_tracked = g_game->current_turn;
        g_ui->turn_start_time = esp_timer_get_time();

        // 重新计算 material score
        g_ui->white_material = 0;
        g_ui->black_material = 0;
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                ChessPiece piece = g_game->board[y][x];
                if (piece.type != PIECE_NONE) {
                    int value = get_piece_value(piece.type);
                    if (piece.color == COLOR_WHITE) {
                        g_ui->white_material += value;
                    } else {
                        g_ui->black_material += value;
                    }
                }
            }
        }

        chess_ui_render_locked();
    }
}

void chess_ui_update_game(const ChessGame* game)
{
    if (game && g_game) {
        ui_lock_acquire();
        chess_ui_update_game_locked(game);
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
    chess_ui_render_locked();
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

    ESP_LOGI(TAG, "Acquiring ui_lock...");
    ui_lock_acquire();
    ESP_LOGI(TAG, "ui_lock acquired");

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
                        strcpy(g_ui->bluetooth_status, "正在连接...");
                        chess_ui_switch_screen(SCREEN_BLUETOOTH_WAITING);
                    }
                }
            } else if (key == KEY_B) {
                ESP_LOGI(TAG, "KEY_B pressed - playing sound...");
                sound_play(SOUND_KEY_CLICK);
                ESP_LOGI(TAG, "Calling bt_chess_stop_scanning...");
                bt_chess_stop_scanning();
                ESP_LOGI(TAG, "Calling chess_ui_switch_screen...");
                chess_ui_switch_screen(SCREEN_BLUETOOTH_MENU);
                ESP_LOGI(TAG, "Screen switched");
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

        case SCREEN_EXIT_CONFIRM:
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
                sound_play(SOUND_MENU_SELECT);
                if (g_ui->menu_index == 0) {
                    // Confirm exit
                    bool is_bluetooth_game = (bt_chess_get_state() == BT_CHESS_STATE_GAME_READY ||
                                             bt_chess_get_state() == BT_CHESS_STATE_PLAYING);
                    if (is_bluetooth_game) {
                        bt_chess_send_resign();
                    }
                    chess_ui_switch_screen(SCREEN_MAIN_MENU);
                } else {
                    // Cancel - go back to game
                    chess_ui_switch_screen(g_ui->previous_screen);
                }
            } else if (key == KEY_B) {
                sound_play(SOUND_KEY_CLICK);
                // Cancel - go back to game
                chess_ui_switch_screen(g_ui->previous_screen);
            }
            break;

        case SCREEN_GAME:
            ESP_LOGI(TAG, "Processing game screen key...");
            // If game is over, only handle B key to go back to menu
            if (chess_game_is_game_over(g_game)) {
                ESP_LOGI(TAG, "Game is over");
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

            ESP_LOGI(TAG, "Getting bluetooth state...");
            // Get bluetooth state first (these are simple variable reads, thread-safe)
            BluetoothChessState bt_state = bt_chess_get_state();
            ESP_LOGI(TAG, "BT state: %d", bt_state);
            bool is_bluetooth_game = (bt_state == BT_CHESS_STATE_GAME_READY ||
                                     bt_state == BT_CHESS_STATE_PLAYING);
            bool is_reconnecting = (bt_state == BT_CHESS_STATE_RECONNECTING);
            bool is_our_turn = true;
            if (is_bluetooth_game) {
                ESP_LOGI(TAG, "Checking if it's our turn...");
                is_our_turn = bt_chess_is_my_turn();
                ESP_LOGI(TAG, "Our turn: %d", is_our_turn);
            }

            // In reconnecting state, only handle A and B keys
            if (is_reconnecting) {
                if (key == KEY_A) {
                    // Try manual reconnect
                    if (bt_chess_try_reconnect()) {
                        sound_play(SOUND_KEY_CLICK);
                        chess_ui_show_message_internal("Reconnecting...", 2000);
                    } else {
                        sound_play(SOUND_INVALID_MOVE);
                        chess_ui_show_message_internal("Reconnect failed", 2000);
                    }
                } else if (key == KEY_B) {
                    sound_play(SOUND_KEY_CLICK);
                    // Cancel reconnection and go back to main menu
                    bt_chess_disconnect();
                    chess_ui_switch_screen(SCREEN_MAIN_MENU);
                }
                break;
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
                ESP_LOGI(TAG, "KEY_A pressed in game screen");
                ChessPiece piece = chess_game_get_piece(g_game, g_ui->cursor_x, g_ui->cursor_y);
                ESP_LOGI(TAG, "Piece at cursor: type=%d, color=%d", piece.type, piece.color);

                // Only allow moves if it's our turn
                if (!is_our_turn && is_bluetooth_game) {
                    ESP_LOGI(TAG, "Not our turn, showing message");
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

                        // Get who is making the move before making it
                        ChessColor moving_color = chess_game_get_current_turn(g_game);

                        // Update the timer for the current player before the move
                        int64_t now = esp_timer_get_time();
                        if (moving_color == COLOR_WHITE) {
                            g_ui->white_elapsed_us += now - g_ui->turn_start_time;
                        } else {
                            g_ui->black_elapsed_us += now - g_ui->turn_start_time;
                        }

                        // Make move
                        if (chess_game_make_move(g_game, g_ui->selected_x, g_ui->selected_y,
                                                g_ui->cursor_x, g_ui->cursor_y, PIECE_QUEEN)) {
                            // Track the last move
                            g_ui->last_move_from_x = g_ui->selected_x;
                            g_ui->last_move_from_y = g_ui->selected_y;
                            g_ui->last_move_to_x = g_ui->cursor_x;
                            g_ui->last_move_to_y = g_ui->cursor_y;
                            g_ui->has_last_move = true;

                            if (is_capture) {
                                sound_play(SOUND_CAPTURE);
                                // Update material score based on piece value
                                int value = get_piece_value(target_piece.type);
                                if (value > 0) {
                                    if (moving_color == COLOR_WHITE) {
                                        g_ui->white_material += value;
                                    } else {
                                        g_ui->black_material += value;
                                    }
                                }
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
                                // 保存游戏状态用于同步
                                bt_chess_save_game_state(g_game);
                                bt_chess_send_move(g_ui->selected_x, g_ui->selected_y,
                                                   g_ui->cursor_x, g_ui->cursor_y, PIECE_QUEEN);
                            }
                        } else {
                            sound_play(SOUND_INVALID_MOVE);
                        }
                        g_ui->has_selection = false;
                        g_ui->selected_x = -1;
                        g_ui->selected_y = -1;

                        // Start timer for the new turn
                        g_ui->turn_start_time = esp_timer_get_time();
                        g_ui->current_turn_tracked = chess_game_get_current_turn(g_game);
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
                // Check if we're in reconnecting state - allow cancel
                BluetoothChessState bt_state = bt_chess_get_state();
                if (bt_state == BT_CHESS_STATE_RECONNECTING) {
                    // Cancel reconnection and go back to main menu
                    bt_chess_disconnect();
                    chess_ui_switch_screen(SCREEN_MAIN_MENU);
                } else if (g_ui->has_selection) {
                    g_ui->has_selection = false;
                    g_ui->selected_x = -1;
                    g_ui->selected_y = -1;
                } else {
                    // Show exit confirmation instead of exiting directly
                    chess_ui_switch_screen(SCREEN_EXIT_CONFIRM);
                }
            }
            break;
    }

    ESP_LOGI(TAG, "Calling chess_ui_render_locked...");
    chess_ui_render_locked();
    ESP_LOGI(TAG, "Releasing ui_lock...");
    ui_lock_release();
    ESP_LOGI(TAG, "Key handler completed");
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
static void chess_ui_render_internal(bool force)
{
    if (!g_ui || !g_ui->sprite_created) return;

    // Throttle rendering to max 30 FPS (33ms per frame) unless forced
    int64_t now = esp_timer_get_time();
    if (!force && now - g_ui->last_render_tick < 33000) {  // 33ms = ~30 FPS
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
        case SCREEN_EXIT_CONFIRM:
            render_exit_confirm();
            break;
    }

    // Render message
    if (g_ui->show_message) {
        render_message();
    }

    // Push sprite to display
    tft_sprite_push(&g_ui->sprite, 0, 0);
}

// Internal render function - assumes lock is already held
void chess_ui_render_locked(void)
{
    chess_ui_render_internal(false);
}

// Internal render function - assumes lock is already held, bypasses throttling
void chess_ui_render_locked_force(void)
{
    chess_ui_render_internal(true);
}

void chess_ui_render(void)
{
    if (!g_ui || !g_ui->sprite_created) return;

    ui_lock_acquire();
    chess_ui_render_locked();
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

    // Found devices - make a local copy to avoid holding the lock during rendering
    FoundBluetoothDevice local_devices[MAX_FOUND_DEVICES];
    int device_count = bt_chess_copy_found_devices(local_devices, MAX_FOUND_DEVICES);
    int max_display = 4;
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
            FoundBluetoothDevice* device = &local_devices[i];
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

static void render_exit_confirm(void)
{
    // Draw dialog background
    tft_sprite_fill_rect(&g_ui->sprite, 20, 30, 120, 70, COLOR_DIALOG);
    tft_sprite_draw_rect(&g_ui->sprite, 20, 30, 120, 70, TFT_WHITE);

    // Title
    int title_x = (TFT_WIDTH - tft_sprite_text_width("Exit Game?", 1)) / 2;
    int text_x = title_x;
    const char* text = "Exit Game?";
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 45, *text, TFT_WHITE, 1);
        text_x += 6;
        text++;
    }

    // Menu items
    for (int i = 0; i < 2; i++) {
        int y = 65 + i * 20;
        bool is_selected = (i == g_ui->menu_index);

        if (is_selected) {
            tft_sprite_fill_rect(&g_ui->sprite, 35, y, 90, 18, TFT_NAVY);
        }

        text_x = 40;
        text = exit_confirm_items[i];
        while (*text) {
            tft_sprite_draw_char(&g_ui->sprite, text_x, y + 5, *text, is_selected ? TFT_YELLOW : TFT_WHITE, 1);
            text_x += 6;
            text++;
        }
    }
}

static void render_game(void)
{
    // Chess board (128x128, centered or left-aligned)
    int board_offset_x = 0;  // Left align to leave room for sidebar
    int board_offset_y = 0;
    int square_size = SQUARE_SIZE;

    // Fill board area background with dark gray
    tft_sprite_fill_rect(&g_ui->sprite, 0, 0, 128, 128, COLOR_BOARD_BG);

    // Draw the board
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            ChessPiece piece = chess_game_get_piece(g_game, x, y);
            bool is_cursor = (x == g_ui->cursor_x && y == g_ui->cursor_y);
            bool is_selected = g_ui->has_selection && (x == g_ui->selected_x && y == g_ui->selected_y);

            // Check if this square was part of the last move
            bool is_last_move_from = (g_ui->has_last_move && x == g_ui->last_move_from_x && y == g_ui->last_move_from_y);
            bool is_last_move_to = (g_ui->has_last_move && x == g_ui->last_move_to_x && y == g_ui->last_move_to_y);
            bool is_last_move = is_last_move_from || is_last_move_to;

            // Draw square
            int px = board_offset_x + x * square_size;
            int py = board_offset_y + y * square_size;
            uint16_t bg_color = ((x + y) % 2 == 0) ? COLOR_LIGHT_SQUARE : COLOR_DARK_SQUARE;

            // Override with last move highlight if needed
            if (is_last_move) {
                bg_color = COLOR_LAST_MOVE;
            }

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

    // Bluetooth connection status indicator (top-right corner)
    BluetoothChessState bt_state = bt_chess_get_state();
    bool is_bluetooth_connected = (bt_state >= BT_CHESS_STATE_CONNECTED && bt_state != BT_CHESS_STATE_RECONNECTING);
    uint16_t bt_status_color = (bt_state == BT_CHESS_STATE_RECONNECTING) ? TFT_YELLOW :
                               (is_bluetooth_connected ? TFT_GREEN : TFT_RED);
    tft_sprite_fill_rect(&g_ui->sprite, sidebar_x + 20, 2, 10, 10, bt_status_color);

    // Turn indicator
    ChessColor turn = chess_game_get_current_turn(g_game);
    tft_sprite_fill_rect(&g_ui->sprite, sidebar_x, 0, 32, 20, COLOR_BACKGROUND);
    tft_sprite_fill_rect(&g_ui->sprite, sidebar_x + 2, 2, 14, 14, (turn == COLOR_WHITE) ? TFT_WHITE : TFT_BLACK);
    tft_sprite_draw_rect(&g_ui->sprite, sidebar_x + 2, 2, 14, 14, TFT_LIGHTGREY);

    // Check indicator
    if (chess_game_is_in_check(g_game, turn)) {
        int check_x = sidebar_x + 2;
        int check_y = 16;
        const char* check_text = "+";
        while (*check_text) {
            tft_sprite_draw_char(&g_ui->sprite, check_x, check_y, *check_text, TFT_RED, 1);
            check_x += 6;
            check_text++;
        }
    }

    // --- White player (top) ---
    // White color indicator
    tft_sprite_fill_rect(&g_ui->sprite, sidebar_x + 2, 22, 10, 10, TFT_WHITE);
    tft_sprite_draw_rect(&g_ui->sprite, sidebar_x + 2, 22, 10, 10, TFT_LIGHTGREY);

    // White material score
    char white_mat_text[8];
    snprintf(white_mat_text, sizeof(white_mat_text), "%d", g_ui->white_material);
    int text_x = sidebar_x + 14;
    const char* text = white_mat_text;
    int i = 0;
    while (text[i]) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 25, text[i], TFT_LIGHTGREY, 1);
        text_x += 6;
        i++;
    }

    // Calculate elapsed time including current turn
    int64_t now = esp_timer_get_time();
    int64_t white_total = g_ui->white_elapsed_us;
    int64_t black_total = g_ui->black_elapsed_us;

    // Add current turn time to respective player (prevent negative values)
    if (turn == COLOR_WHITE) {
        int64_t elapsed = now - g_ui->turn_start_time;
        white_total += (elapsed > 0) ? elapsed : 0;
    } else {
        int64_t elapsed = now - g_ui->turn_start_time;
        black_total += (elapsed > 0) ? elapsed : 0;
    }

    // Ensure totals don't go negative
    if (white_total < 0) white_total = 0;
    if (black_total < 0) black_total = 0;

    // White timer (MM:SS format)
    int white_sec = (int)(white_total / 1000000);
    int white_min = white_sec / 60;
    white_sec = white_sec % 60;
    // Clamp values to prevent overflow
    if (white_min < 0) white_min = 0;
    if (white_min > 99) white_min = 99;
    if (white_sec < 0) white_sec = 0;
    if (white_sec > 59) white_sec = 59;
    char white_timer_text[16];
    snprintf(white_timer_text, sizeof(white_timer_text), "%02d:%02d", white_min, white_sec);
    text_x = sidebar_x + 2;
    text = white_timer_text;
    i = 0;
    while (text[i]) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 38, text[i], turn == COLOR_WHITE ? TFT_YELLOW : TFT_LIGHTGREY, 1);
        text_x += 6;
        i++;
    }

    // --- Black player (middle) ---
    // Black color indicator
    tft_sprite_fill_rect(&g_ui->sprite, sidebar_x + 2, 52, 10, 10, TFT_BLACK);
    tft_sprite_draw_rect(&g_ui->sprite, sidebar_x + 2, 52, 10, 10, TFT_LIGHTGREY);

    // Black material score
    char black_mat_text[8];
    snprintf(black_mat_text, sizeof(black_mat_text), "%d", g_ui->black_material);
    text_x = sidebar_x + 14;
    text = black_mat_text;
    i = 0;
    while (text[i]) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 55, text[i], TFT_LIGHTGREY, 1);
        text_x += 6;
        i++;
    }

    // Black timer (MM:SS format)
    int black_sec = (int)(black_total / 1000000);
    int black_min = black_sec / 60;
    black_sec = black_sec % 60;
    // Clamp values to prevent overflow
    if (black_min < 0) black_min = 0;
    if (black_min > 99) black_min = 99;
    if (black_sec < 0) black_sec = 0;
    if (black_sec > 59) black_sec = 59;
    char black_timer_text[16];
    snprintf(black_timer_text, sizeof(black_timer_text), "%02d:%02d", black_min, black_sec);
    text_x = sidebar_x + 2;
    text = black_timer_text;
    i = 0;
    while (text[i]) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 68, text[i], turn == COLOR_BLACK ? TFT_YELLOW : TFT_LIGHTGREY, 1);
        text_x += 6;
        i++;
    }

    // Move count
    char move_text[16];
    snprintf(move_text, sizeof(move_text), "M:%d", g_game->fullmove_number);
    text_x = sidebar_x + 4;
    text = move_text;
    while (*text) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 85, *text, TFT_LIGHTGREY, 1);
        text_x += 6;
        text++;
    }

    // Bluetooth game indicator - show OPP first, then YOU
    if (bt_state >= BT_CHESS_STATE_GAME_READY) {
        // Show if it's our turn - OPP is first (red when it's opponent's turn, green when it's ours)
        uint16_t turn_color = bt_chess_is_my_turn() ? TFT_GREEN : TFT_RED;
        tft_sprite_fill_rect(&g_ui->sprite, sidebar_x, 98, 32, 14, turn_color);
        const char* turn_text = bt_chess_is_my_turn() ? "YOU" : "OPP";
        text_x = sidebar_x + 6;
        i = 0;
        while (turn_text[i]) {
            tft_sprite_draw_char(&g_ui->sprite, text_x, 102, turn_text[i], TFT_BLACK, 1);
            text_x += 6;
            i++;
        }
    }

    // Instructions
    text_x = sidebar_x + 2;
    text = "B:Exit";
    i = 0;
    while (text[i]) {
        tft_sprite_draw_char(&g_ui->sprite, text_x, 118, text[i], TFT_LIGHTGREY, 1);
        text_x += 6;
        i++;
    }

    // Reconnection status overlay
    BluetoothChessState bt_state_now = bt_chess_get_state();
    if (bt_state_now == BT_CHESS_STATE_RECONNECTING) {
        // Draw reconnecting dialog
        tft_sprite_fill_rect(&g_ui->sprite, 10, 20, 140, 65, COLOR_DIALOG);
        tft_sprite_draw_rect(&g_ui->sprite, 10, 20, 140, 65, TFT_YELLOW);

        // Reconnecting text
        const char* recon_title = "Connection Lost!";
        int recon_title_x = (TFT_WIDTH - tft_sprite_text_width(recon_title, 1)) / 2;
        text_x = recon_title_x;
        text = recon_title;
        while (*text) {
            tft_sprite_draw_char(&g_ui->sprite, text_x, 35, *text, TFT_YELLOW, 1);
            text_x += 6;
            text++;
        }

        // 根据角色显示不同提示
        const char* recon_hint;
        if (bt_chess_get_role() == BT_CHESS_ROLE_HOST) {
            recon_hint = "Waiting for guest";
        } else {
            recon_hint = "Press A to retry";
        }
        int recon_hint_x = (TFT_WIDTH - tft_sprite_text_width(recon_hint, 1)) / 2;
        text_x = recon_hint_x;
        text = recon_hint;
        while (*text) {
            tft_sprite_draw_char(&g_ui->sprite, text_x, 50, *text, TFT_LIGHTGREY, 1);
            text_x += 6;
            text++;
        }

        const char* cancel_hint = "Press B to exit";
        int cancel_hint_x = (TFT_WIDTH - tft_sprite_text_width(cancel_hint, 1)) / 2;
        text_x = cancel_hint_x;
        text = cancel_hint;
        while (*text) {
            tft_sprite_draw_char(&g_ui->sprite, text_x, 65, *text, TFT_LIGHTGREY, 1);
            text_x += 6;
            text++;
        }
    }

    // Game over overlay
    if (chess_game_is_game_over(g_game)) {
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
