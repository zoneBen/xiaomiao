#ifndef GOMOKU_H
#define GOMOKU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 五子棋棋盘大小 - 为了适配屏幕使用 11x11
#define GOMOKU_BOARD_SIZE 11

// 棋子类型
typedef enum {
    GOMOKU_EMPTY = 0,
    GOMOKU_BLACK = 1,
    GOMOKU_WHITE = 2
} GomokuPiece;

// 游戏状态
typedef struct {
    GomokuPiece board[GOMOKU_BOARD_SIZE][GOMOKU_BOARD_SIZE];
    GomokuPiece current_player;
    bool game_over;
    GomokuPiece winner;
    int move_count;
} GomokuGame;

// 初始化游戏
void gomoku_game_init(GomokuGame* game);
void gomoku_game_reset(GomokuGame* game);

// 落子 - 返回是否成功
bool gomoku_place_piece(GomokuGame* game, int x, int y);

// 检查是否获胜
bool gomoku_check_win(GomokuGame* game, int x, int y, GomokuPiece player);

// 检查位置是否有效
bool gomoku_is_valid_position(int x, int y);

#endif // GOMOKU_H