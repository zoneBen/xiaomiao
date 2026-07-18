#ifndef GO_H
#define GO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 9x9 围棋
#define GO_BOARD_SIZE 9

// 棋子类型
typedef enum {
    GO_EMPTY = 0,
    GO_BLACK = 1,
    GO_WHITE = 2
} GoPiece;

// 游戏状态
typedef struct {
    GoPiece board[GO_BOARD_SIZE][GO_BOARD_SIZE];
    GoPiece current_player;
    bool game_over;
    GoPiece winner;
    int move_count;
    // 打劫位置 - 上一步提子的位置，禁止立即提回
    int ko_x;
    int ko_y;
    // 上一步的位置
    int last_x;
    int last_y;
    // 双方连续弃权次数
    int pass_count;
} GoGame;

// 初始化游戏
void go_game_init(GoGame* game);
void go_game_reset(GoGame* game);

// 落子 - 返回是否成功
bool go_place_piece(GoGame* game, int x, int y);

// 弃权
void go_pass(GoGame* game);

// 检查位置是否有效
bool go_is_valid_position(int x, int y);

// 计算一块棋的气数
int go_count_liberties(GoGame* game, int x, int y, bool visited[GO_BOARD_SIZE][GO_BOARD_SIZE]);

// 提子 - 返回提子数量
int go_remove_group(GoGame* game, int x, int y, GoPiece piece);

#endif // GO_H