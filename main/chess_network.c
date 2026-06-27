#include "chess_network.h"
#include "network.h"
#include "chess.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "chess_net";

#define CHESS_TOPIC_BASE "chess/"
#define CHESS_TOPIC_LOBBY CHESS_TOPIC_BASE "lobby"
#define CHESS_TOPIC_GAME_PREFIX CHESS_TOPIC_BASE "game/"

typedef struct {
    ChessNetworkContext net_ctx;
    chess_network_state_callback_t state_callback;
    chess_network_invite_callback_t invite_callback;
    chess_network_move_callback_t move_callback;
    chess_network_chat_callback_t chat_callback;
    chess_network_game_over_callback_t game_over_callback;
    bool initialized;
} ChessNetworkInternal;

static ChessNetworkInternal* g_ctx = NULL;

static void update_state(ChessNetworkState new_state) {
    if (g_ctx && g_ctx->net_ctx.state != new_state) {
        g_ctx->net_ctx.state = new_state;
        if (g_ctx->state_callback) {
            g_ctx->state_callback(new_state);
        }
    }
}

static void generate_game_topic(char* buffer, size_t buffer_size, const char* id1, const char* id2) {
    if (strcmp(id1, id2) < 0) {
        snprintf(buffer, buffer_size, "%s%s_%s", CHESS_TOPIC_GAME_PREFIX, id1, id2);
    } else {
        snprintf(buffer, buffer_size, "%s%s_%s", CHESS_TOPIC_GAME_PREFIX, id2, id1);
    }
}

static void network_message_handler(const char* topic, const char* payload, size_t length) {
    if (!g_ctx || !payload || length == 0) {
        return;
    }

    cJSON* root = cJSON_ParseWithLength(payload, length);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }

    cJSON* type_item = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsNumber(type_item)) {
        cJSON_Delete(root);
        return;
    }

    ChessNetworkMessageType type = (ChessNetworkMessageType)type_item->valueint;
    cJSON* from_item = cJSON_GetObjectItem(root, "from");
    const char* from_id = cJSON_IsString(from_item) ? from_item->valuestring : "";

    switch (type) {
        case CHESS_NET_MSG_HELLO: {
            ESP_LOGI(TAG, "Received hello from %s", from_id);
            break;
        }
        case CHESS_NET_MSG_SEARCH: {
            ESP_LOGI(TAG, "Player %s is searching", from_id);
            if (g_ctx->net_ctx.state == CHESS_NET_STATE_SEARCHING &&
                strcmp(from_id, g_ctx->net_ctx.player_id) != 0) {
                if (g_ctx->invite_callback) {
                    g_ctx->invite_callback(from_id);
                }
            }
            break;
        }
        case CHESS_NET_MSG_INVITE: {
            ESP_LOGI(TAG, "Received invite from %s", from_id);
            if (g_ctx->net_ctx.state == CHESS_NET_STATE_SEARCHING ||
                g_ctx->net_ctx.state == CHESS_NET_STATE_IDLE) {
                if (g_ctx->invite_callback) {
                    g_ctx->invite_callback(from_id);
                }
            }
            break;
        }
        case CHESS_NET_MSG_ACCEPT: {
            ESP_LOGI(TAG, "Invite accepted by %s", from_id);
            if (g_ctx->net_ctx.state == CHESS_NET_STATE_WAITING_ACCEPT) {
                strncpy(g_ctx->net_ctx.opponent_id, from_id, sizeof(g_ctx->net_ctx.opponent_id) - 1);
                generate_game_topic(g_ctx->net_ctx.game_topic, sizeof(g_ctx->net_ctx.game_topic),
                                    g_ctx->net_ctx.player_id, from_id);
                network_mqtt_subscribe(g_ctx->net_ctx.game_topic);
                g_ctx->net_ctx.role = CHESS_NET_PLAYER_LOCAL;
                g_ctx->net_ctx.local_color = COLOR_WHITE;
                g_ctx->net_ctx.game_ready = true;
                update_state(CHESS_NET_STATE_PLAYING);
            }
            break;
        }
        case CHESS_NET_MSG_REJECT: {
            ESP_LOGI(TAG, "Invite rejected by %s", from_id);
            update_state(CHESS_NET_STATE_SEARCHING);
            break;
        }
        case CHESS_NET_MSG_MOVE: {
            if (strcmp(from_id, g_ctx->net_ctx.opponent_id) != 0) {
                break;
            }
            cJSON* from_x_item = cJSON_GetObjectItem(root, "from_x");
            cJSON* from_y_item = cJSON_GetObjectItem(root, "from_y");
            cJSON* to_x_item = cJSON_GetObjectItem(root, "to_x");
            cJSON* to_y_item = cJSON_GetObjectItem(root, "to_y");
            cJSON* promotion_item = cJSON_GetObjectItem(root, "promotion");

            if (cJSON_IsNumber(from_x_item) && cJSON_IsNumber(from_y_item) &&
                cJSON_IsNumber(to_x_item) && cJSON_IsNumber(to_y_item)) {
                ChessPieceType promotion = PIECE_NONE;
                if (cJSON_IsNumber(promotion_item)) {
                    promotion = (ChessPieceType)promotion_item->valueint;
                }
                if (g_ctx->move_callback) {
                    g_ctx->move_callback(from_x_item->valueint, from_y_item->valueint,
                                        to_x_item->valueint, to_y_item->valueint,
                                        promotion);
                }
            }
            break;
        }
        case CHESS_NET_MSG_CHAT: {
            cJSON* msg_item = cJSON_GetObjectItem(root, "message");
            if (cJSON_IsString(msg_item) && g_ctx->chat_callback) {
                g_ctx->chat_callback(msg_item->valuestring);
            }
            break;
        }
        case CHESS_NET_MSG_RESIGN: {
            if (strcmp(from_id, g_ctx->net_ctx.opponent_id) == 0) {
                if (g_ctx->game_over_callback) {
                    g_ctx->game_over_callback(g_ctx->net_ctx.local_color);
                }
                update_state(CHESS_NET_STATE_GAME_OVER);
            }
            break;
        }
        case CHESS_NET_MSG_DRAW_OFFER: {
            ESP_LOGI(TAG, "Draw offer from %s", from_id);
            break;
        }
        case CHESS_NET_MSG_DRAW_ACCEPT: {
            if (strcmp(from_id, g_ctx->net_ctx.opponent_id) == 0) {
                if (g_ctx->game_over_callback) {
                    g_ctx->game_over_callback(COLOR_WHITE);
                }
                update_state(CHESS_NET_STATE_GAME_OVER);
            }
            break;
        }
        case CHESS_NET_MSG_SYNC: {
            ESP_LOGI(TAG, "Received sync from %s", from_id);
            break;
        }
        case CHESS_NET_MSG_GAME_OVER: {
            ESP_LOGI(TAG, "Game over message from %s", from_id);
            break;
        }
        default:
            break;
    }

    cJSON_Delete(root);
}

static bool send_json(const char* topic, cJSON* json) {
    char* payload = cJSON_PrintUnformatted(json);
    bool success = false;
    if (payload) {
        success = network_mqtt_publish(topic, payload, strlen(payload));
        cJSON_free(payload);
    }
    return success;
}

void chess_network_init(void) {
    if (g_ctx) {
        return;
    }

    g_ctx = (ChessNetworkInternal*)calloc(1, sizeof(ChessNetworkInternal));
    if (!g_ctx) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return;
    }

    g_ctx->net_ctx.state = CHESS_NET_STATE_IDLE;
    g_ctx->net_ctx.role = CHESS_NET_PLAYER_LOCAL;
    g_ctx->net_ctx.local_color = COLOR_WHITE;
    g_ctx->initialized = true;

    network_set_message_callback(network_message_handler);

    snprintf(g_ctx->net_ctx.player_id, sizeof(g_ctx->net_ctx.player_id),
             "player_%08lx", esp_random());
}

void chess_network_deinit(void) {
    if (!g_ctx) {
        return;
    }

    if (strlen(g_ctx->net_ctx.game_topic) > 0) {
        network_mqtt_unsubscribe(g_ctx->net_ctx.game_topic);
    }
    network_mqtt_unsubscribe(CHESS_TOPIC_LOBBY);

    free(g_ctx);
    g_ctx = NULL;
}

void chess_network_set_player_id(const char* id) {
    if (g_ctx && id) {
        strncpy(g_ctx->net_ctx.player_id, id, sizeof(g_ctx->net_ctx.player_id) - 1);
    }
}

const char* chess_network_get_player_id(void) {
    if (g_ctx) {
        return g_ctx->net_ctx.player_id;
    }
    return "";
}

bool chess_network_start_search(void) {
    if (!g_ctx || !network_is_connected()) {
        return false;
    }

    network_mqtt_subscribe(CHESS_TOPIC_LOBBY);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "type", CHESS_NET_MSG_SEARCH);
    cJSON_AddStringToObject(root, "from", g_ctx->net_ctx.player_id);

    bool success = send_json(CHESS_TOPIC_LOBBY, root);
    cJSON_Delete(root);

    if (success) {
        update_state(CHESS_NET_STATE_SEARCHING);
    }

    return success;
}

void chess_network_stop_search(void) {
    if (!g_ctx) {
        return;
    }

    network_mqtt_unsubscribe(CHESS_TOPIC_LOBBY);
    update_state(CHESS_NET_STATE_IDLE);
}

bool chess_network_send_invite(const char* opponent_id) {
    if (!g_ctx || !opponent_id || !network_is_connected()) {
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "type", CHESS_NET_MSG_INVITE);
    cJSON_AddStringToObject(root, "from", g_ctx->net_ctx.player_id);
    cJSON_AddStringToObject(root, "to", opponent_id);

    bool success = send_json(CHESS_TOPIC_LOBBY, root);
    cJSON_Delete(root);

    if (success) {
        strncpy(g_ctx->net_ctx.opponent_id, opponent_id, sizeof(g_ctx->net_ctx.opponent_id) - 1);
        update_state(CHESS_NET_STATE_WAITING_ACCEPT);
    }

    return success;
}

bool chess_network_accept_invite(const char* opponent_id) {
    if (!g_ctx || !opponent_id || !network_is_connected()) {
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "type", CHESS_NET_MSG_ACCEPT);
    cJSON_AddStringToObject(root, "from", g_ctx->net_ctx.player_id);
    cJSON_AddStringToObject(root, "to", opponent_id);

    bool success = send_json(CHESS_TOPIC_LOBBY, root);
    cJSON_Delete(root);

    if (success) {
        strncpy(g_ctx->net_ctx.opponent_id, opponent_id, sizeof(g_ctx->net_ctx.opponent_id) - 1);
        generate_game_topic(g_ctx->net_ctx.game_topic, sizeof(g_ctx->net_ctx.game_topic),
                            g_ctx->net_ctx.player_id, opponent_id);
        network_mqtt_subscribe(g_ctx->net_ctx.game_topic);
        g_ctx->net_ctx.role = CHESS_NET_PLAYER_REMOTE;
        g_ctx->net_ctx.local_color = COLOR_BLACK;
        g_ctx->net_ctx.game_ready = true;
        update_state(CHESS_NET_STATE_PLAYING);
    }

    return success;
}

bool chess_network_reject_invite(const char* opponent_id) {
    if (!g_ctx || !opponent_id || !network_is_connected()) {
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "type", CHESS_NET_MSG_REJECT);
    cJSON_AddStringToObject(root, "from", g_ctx->net_ctx.player_id);
    cJSON_AddStringToObject(root, "to", opponent_id);

    bool success = send_json(CHESS_TOPIC_LOBBY, root);
    cJSON_Delete(root);

    if (success) {
        update_state(CHESS_NET_STATE_SEARCHING);
    }

    return success;
}

bool chess_network_send_move(int from_x, int from_y, int to_x, int to_y, ChessPieceType promotion) {
    if (!g_ctx || !network_is_connected() || !g_ctx->net_ctx.game_ready) {
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "type", CHESS_NET_MSG_MOVE);
    cJSON_AddStringToObject(root, "from", g_ctx->net_ctx.player_id);
    cJSON_AddNumberToObject(root, "from_x", from_x);
    cJSON_AddNumberToObject(root, "from_y", from_y);
    cJSON_AddNumberToObject(root, "to_x", to_x);
    cJSON_AddNumberToObject(root, "to_y", to_y);
    if (promotion != PIECE_NONE) {
        cJSON_AddNumberToObject(root, "promotion", promotion);
    }

    bool success = send_json(g_ctx->net_ctx.game_topic, root);
    cJSON_Delete(root);

    return success;
}

bool chess_network_send_chat(const char* message) {
    if (!g_ctx || !message || !network_is_connected() || !g_ctx->net_ctx.game_ready) {
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "type", CHESS_NET_MSG_CHAT);
    cJSON_AddStringToObject(root, "from", g_ctx->net_ctx.player_id);
    cJSON_AddStringToObject(root, "message", message);

    bool success = send_json(g_ctx->net_ctx.game_topic, root);
    cJSON_Delete(root);

    return success;
}

bool chess_network_send_resign(void) {
    if (!g_ctx || !network_is_connected() || !g_ctx->net_ctx.game_ready) {
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "type", CHESS_NET_MSG_RESIGN);
    cJSON_AddStringToObject(root, "from", g_ctx->net_ctx.player_id);

    bool success = send_json(g_ctx->net_ctx.game_topic, root);
    cJSON_Delete(root);

    if (success) {
        update_state(CHESS_NET_STATE_GAME_OVER);
    }

    return success;
}

bool chess_network_send_draw_offer(void) {
    if (!g_ctx || !network_is_connected() || !g_ctx->net_ctx.game_ready) {
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "type", CHESS_NET_MSG_DRAW_OFFER);
    cJSON_AddStringToObject(root, "from", g_ctx->net_ctx.player_id);

    bool success = send_json(g_ctx->net_ctx.game_topic, root);
    cJSON_Delete(root);

    return success;
}

bool chess_network_accept_draw(void) {
    if (!g_ctx || !network_is_connected() || !g_ctx->net_ctx.game_ready) {
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "type", CHESS_NET_MSG_DRAW_ACCEPT);
    cJSON_AddStringToObject(root, "from", g_ctx->net_ctx.player_id);

    bool success = send_json(g_ctx->net_ctx.game_topic, root);
    cJSON_Delete(root);

    if (success) {
        update_state(CHESS_NET_STATE_GAME_OVER);
    }

    return success;
}

ChessNetworkState chess_network_get_state(void) {
    if (g_ctx) {
        return g_ctx->net_ctx.state;
    }
    return CHESS_NET_STATE_IDLE;
}

ChessNetworkPlayerRole chess_network_get_role(void) {
    if (g_ctx) {
        return g_ctx->net_ctx.role;
    }
    return CHESS_NET_PLAYER_LOCAL;
}

ChessColor chess_network_get_local_color(void) {
    if (g_ctx) {
        return g_ctx->net_ctx.local_color;
    }
    return COLOR_WHITE;
}

bool chess_network_is_my_turn(void) {
    if (!g_ctx || !g_ctx->net_ctx.game_ready) {
        return false;
    }
    return true;
}

void chess_network_set_state_callback(chess_network_state_callback_t callback) {
    if (g_ctx) {
        g_ctx->state_callback = callback;
    }
}

void chess_network_set_invite_callback(chess_network_invite_callback_t callback) {
    if (g_ctx) {
        g_ctx->invite_callback = callback;
    }
}

void chess_network_set_move_callback(chess_network_move_callback_t callback) {
    if (g_ctx) {
        g_ctx->move_callback = callback;
    }
}

void chess_network_set_chat_callback(chess_network_chat_callback_t callback) {
    if (g_ctx) {
        g_ctx->chat_callback = callback;
    }
}

void chess_network_set_game_over_callback(chess_network_game_over_callback_t callback) {
    if (g_ctx) {
        g_ctx->game_over_callback = callback;
    }
}

void chess_network_sync_game(const ChessGame* game) {
}
