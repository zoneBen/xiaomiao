#include "go.h"
#include <string.h>
#include "esp_log.h"

static const char* TAG = "Go";

bool go_is_valid_position(int x, int y)
{
    return x >= 0 && x < GO_BOARD_SIZE && y >= 0 && y < GO_BOARD_SIZE;
}

void go_game_init(GoGame* game)
{
    go_game_reset(game);
}

void go_game_reset(GoGame* game)
{
    memset(game, 0, sizeof(GoGame));
    game->current_player = GO_BLACK; // 黑棋先行
    game->game_over = false;
    game->winner = GO_EMPTY;
    game->move_count = 0;
    game->ko_x = -1;
    game->ko_y = -1;
    game->last_x = -1;
    game->last_y = -1;
    game->pass_count = 0;
}

// 辅助函数：标记访问过的位置，用于气数计算
static void go_mark_group(GoGame* game, int x, int y, GoPiece piece,
                         bool visited[GO_BOARD_SIZE][GO_BOARD_SIZE])
{
    if (!go_is_valid_position(x, y)) return;
    if (visited[y][x]) return;
    if (game->board[y][x] != piece) return;

    visited[y][x] = true;

    go_mark_group(game, x - 1, y, piece, visited);
    go_mark_group(game, x + 1, y, piece, visited);
    go_mark_group(game, x, y - 1, piece, visited);
    go_mark_group(game, x, y + 1, piece, visited);
}

// 计算一块棋的气数
int go_count_liberties(GoGame* game, int x, int y, bool visited[GO_BOARD_SIZE][GO_BOARD_SIZE])
{
    // 初始化访问标记
    bool group_visited[GO_BOARD_SIZE][GO_BOARD_SIZE] = {false};
    go_mark_group(game, x, y, game->board[y][x], group_visited);

    int liberties = 0;
    bool liberty_visited[GO_BOARD_SIZE][GO_BOARD_SIZE] = {false};

    for (int cy = 0; cy < GO_BOARD_SIZE; cy++) {
        for (int cx = 0; cx < GO_BOARD_SIZE; cx++) {
            if (group_visited[cy][cx]) {
                int directions[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (int d = 0; d < 4; d++) {
                    int nx = cx + directions[d][0];
                    int ny = cy + directions[d][1];
                    if (go_is_valid_position(nx, ny)) {
                        if (game->board[ny][nx] == GO_EMPTY && !liberty_visited[ny][nx]) {
                            liberty_visited[ny][nx] = true;
                            liberties++;
                        }
                    }
                }
            }
        }
    }

    if (visited != NULL) {
        memcpy(visited, group_visited, sizeof(group_visited));
    }

    return liberties;
}

// 提子 - 返回提子数量
int go_remove_group(GoGame* game, int x, int y, GoPiece piece)
{
    if (!go_is_valid_position(x, y)) return 0;
    if (game->board[y][x] != piece) return 0;

    bool visited[GO_BOARD_SIZE][GO_BOARD_SIZE] = {false};
    go_mark_group(game, x, y, piece, visited);

    int count = 0;
    for (int cy = 0; cy < GO_BOARD_SIZE; cy++) {
        for (int cx = 0; cx < GO_BOARD_SIZE; cx++) {
            if (visited[cy][cx]) {
                game->board[cy][cx] = GO_EMPTY;
                count++;
            }
        }
    }

    return count;
}

bool go_place_piece(GoGame* game, int x, int y)
{
    if (!go_is_valid_position(x, y)) return false;
    if (game->board[y][x] != GO_EMPTY) return false;
    if (game->game_over) return false;

    // 检查打劫 - 禁止立即提回
    if (x == game->ko_x && y == game->ko_y) {
        return false;
    }

    GoPiece opponent = (game->current_player == GO_BLACK) ? GO_WHITE : GO_BLACK;

    // 模拟落子
    GoGame temp = *game;
    temp.board[y][x] = temp.current_player;

    // 先看是否能提掉对方的棋
    int captured = 0;
    int directions[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int d = 0; d < 4; d++) {
        int nx = x + directions[d][0];
        int ny = y + directions[d][1];
        if (go_is_valid_position(nx, ny) && temp.board[ny][nx] == opponent) {
            if (go_count_liberties(&temp, nx, ny, NULL) == 0) {
                captured += go_remove_group(&temp, nx, ny, opponent);
            }
        }
    }

    // 检查自己是否有气 - 禁着点（自杀）
    bool self_has_liberty = (go_count_liberties(&temp, x, y, NULL) > 0);
    if (!self_has_liberty && captured == 0) {
        return false;
    }

    // 落子有效，执行真正的落子
    game->board[y][x] = game->current_player;

    // 提掉对方没有气的棋
    captured = 0;
    int captured_x = -1, captured_y = -1;
    for (int d = 0; d < 4; d++) {
        int nx = x + directions[d][0];
        int ny = y + directions[d][1];
        if (go_is_valid_position(nx, ny) && game->board[ny][nx] == opponent) {
            if (go_count_liberties(game, nx, ny, NULL) == 0) {
                if (captured == 0) {
                    captured_x = nx;
                    captured_y = ny;
                }
                captured += go_remove_group(game, nx, ny, opponent);
            }
        }
    }

    // 处理打劫 - 如果只提了一个子，且周围情况符合打劫
    game->ko_x = -1;
    game->ko_y = -1;
    if (captured == 1) {
        bool is_ko = true;
        GoGame check_game = *game;
        check_game.board[captured_y][captured_x] = opponent;
        if (go_count_liberties(&check_game, x, y, NULL) == 0) {
            game->ko_x = captured_x;
            game->ko_y = captured_y;
        }
    }

    // 记录上一步
    game->last_x = x;
    game->last_y = y;

    // 重置弃权计数
    game->pass_count = 0;

    // 切换玩家
    game->current_player = opponent;
    game->move_count++;

    return true;
}

void go_pass(GoGame* game)
{
    if (game->game_over) return;

    game->pass_count++;

    // 双方都弃权，游戏结束
    if (game->pass_count >= 2) {
        game->game_over = true;
        game->winner = GO_EMPTY; // 简化处理，不计算最终胜负
    } else {
        GoPiece opponent = (game->current_player == GO_BLACK) ? GO_WHITE : GO_BLACK;
        game->current_player = opponent;
    }

    game->ko_x = -1;
    game->ko_y = -1;
    game->last_x = -1;
    game->last_y = -1;
}