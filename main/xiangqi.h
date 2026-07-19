#ifndef XIANGQI_H
#define XIANGQI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 中国象棋棋盘大小：9列 x 10行
#define XIANGQI_BOARD_WIDTH 9
#define XIANGQI_BOARD_HEIGHT 10

// 棋子类型
typedef enum {
    XIANGQI_PIECE_NONE = 0,

    // 红方棋子
    XIANGQI_PIECE_KING,      // 将
    XIANGQI_PIECE_ADVISOR,   // 士
    XIANGQI_PIECE_ELEPHANT,  // 象
    XIANGQI_PIECE_HORSE,      // 马
    XIANGQI_PIECE_CHARIOT,    // 车
    XIANGQI_PIECE_CANNON,     // 炮
    XIANGQI_PIECE_PAWN,       // 兵

    // 黑方棋子
    XIANGQI_PIECE_KING_B,
    XIANGQI_PIECE_ADVISOR_B,
    XIANGQI_PIECE_ELEPHANT_B,
    XIANGQI_PIECE_HORSE_B,
    XIANGQI_PIECE_CHARIOT_B,
    XIANGQI_PIECE_CANNON_B,
    XIANGQI_PIECE_PAWN_B
} XiangqiPieceType;

// 棋子颜色
typedef enum {
    XIANGQI_COLOR_RED = 0,
    XIANGQI_COLOR_BLACK,
    XIANGQI_COLOR_NONE
} XiangqiColor;

// 棋子结构
typedef struct {
    XiangqiPieceType type;
    XiangqiColor color;
} XiangqiPiece;

// 移动记录
typedef struct {
    int8_t from_x;
    int8_t from_y;
    int8_t to_x;
    int8_t to_y;
    XiangqiPiece captured_piece;
} XiangqiMove;

// 游戏状态
typedef struct {
    XiangqiPiece board[XIANGQI_BOARD_HEIGHT][XIANGQI_BOARD_WIDTH];
    XiangqiColor current_turn;
    XiangqiMove last_move;
    XiangqiColor winner;
    bool game_over;
    int move_count;
} XiangqiGame;

// ==================== 函数声明 ====================

// 初始化游戏
void xiangqi_game_init(XiangqiGame* game);
void xiangqi_game_reset(XiangqiGame* game);

// 棋子操作
XiangqiPiece xiangqi_get_piece(XiangqiGame* game, int x, int y);
XiangqiColor xiangqi_get_piece_color(XiangqiPiece piece);
bool xiangqi_is_red_piece(XiangqiPieceType type);
bool xiangqi_is_black_piece(XiangqiPieceType type);

// 移动验证
bool xiangqi_is_valid_move(XiangqiGame* game, int from_x, int from_y, int to_x, int to_y);
bool xiangqi_make_move(XiangqiGame* game, int from_x, int from_y, int to_x, int to_y);

// 游戏状态
XiangqiColor xiangqi_get_current_turn(XiangqiGame* game);
bool xiangqi_is_game_over(XiangqiGame* game);
XiangqiColor xiangqi_get_winner(XiangqiGame* game);
bool xiangqi_is_in_check(XiangqiGame* game, XiangqiColor color);
bool xiangqi_is_checkmate(XiangqiGame* game, XiangqiColor color);

#endif // XIANGQI_H
