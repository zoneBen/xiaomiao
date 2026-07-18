#include "gomoku.h"
#include <string.h>
#include "esp_log.h"

static const char* TAG = "Gomoku";

bool gomoku_is_valid_position(int x, int y)
{
    return x >= 0 && x < GOMOKU_BOARD_SIZE && y >= 0 && y < GOMOKU_BOARD_SIZE;
}

void gomoku_game_init(GomokuGame* game)
{
    gomoku_game_reset(game);
}

void gomoku_game_reset(GomokuGame* game)
{
    memset(game, 0, sizeof(GomokuGame));
    game->current_player = GOMOKU_BLACK; // 黑棋先行
    game->game_over = false;
    game->winner = GOMOKU_EMPTY;
    game->move_count = 0;
}

bool gomoku_place_piece(GomokuGame* game, int x, int y)
{
    if (!gomoku_is_valid_position(x, y)) return false;
    if (game->board[y][x] != GOMOKU_EMPTY) return false;
    if (game->game_over) return false;

    game->board[y][x] = game->current_player;
    game->move_count++;

    // 检查是否获胜
    if (gomoku_check_win(game, x, y, game->current_player)) {
        game->game_over = true;
        game->winner = game->current_player;
    } else if (game->move_count >= GOMOKU_BOARD_SIZE * GOMOKU_BOARD_SIZE) {
        // 平局
        game->game_over = true;
        game->winner = GOMOKU_EMPTY;
    } else {
        // 切换玩家
        game->current_player = (game->current_player == GOMOKU_BLACK) ?
                               GOMOKU_WHITE : GOMOKU_BLACK;
    }

    return true;
}

bool gomoku_check_win(GomokuGame* game, int x, int y, GomokuPiece player)
{
    // 四个方向：横、竖、左斜、右斜
    int directions[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};

    for (int d = 0; d < 4; d++) {
        int dx = directions[d][0];
        int dy = directions[d][1];
        int count = 1; // 当前棋子算1个

        // 正方向
        for (int i = 1; i < 5; i++) {
            int nx = x + dx * i;
            int ny = y + dy * i;
            if (!gomoku_is_valid_position(nx, ny) || game->board[ny][nx] != player) {
                break;
            }
            count++;
        }

        // 反方向
        for (int i = 1; i < 5; i++) {
            int nx = x - dx * i;
            int ny = y - dy * i;
            if (!gomoku_is_valid_position(nx, ny) || game->board[ny][nx] != player) {
                break;
            }
            count++;
        }

        if (count >= 5) {
            return true;
        }
    }

    return false;
}