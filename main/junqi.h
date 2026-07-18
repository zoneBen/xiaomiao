#ifndef JUNQI_H
#define JUNQI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 军棋棋盘大小 - 简化版本：9行7列
#define JUNQI_BOARD_WIDTH 7
#define JUNQI_BOARD_HEIGHT 9

// 棋子类型
typedef enum {
    JUNQI_PIECE_NONE = 0,
    // 红方
    JUNQI_PIECE_RED_SILING = 1,     // 司令
    JUNQI_PIECE_RED_JUNZHANG = 2,   // 军长
    JUNQI_PIECE_RED_SHIZHANG = 3,   // 师长
    JUNQI_PIECE_RED_LVZHANG = 4,    // 旅长
    JUNQI_PIECE_RED_TUANZHANG = 5,  // 团长
    JUNQI_PIECE_RED_YINGZHANG = 6,  // 营长
    JUNQI_PIECE_RED_LIANZHANG = 7,  // 连长
    JUNQI_PIECE_RED_PAIZHANG = 8,   // 排长
    JUNQI_PIECE_RED_GONGBING = 9,   // 工兵
    JUNQI_PIECE_RED_DILEI = 10,     // 地雷
    JUNQI_PIECE_RED_ZHADAN = 11,    // 炸弹
    JUNQI_PIECE_RED_JUNQI = 12,     // 军旗
    // 蓝方
    JUNQI_PIECE_BLUE_SILING = 13,   // 司令
    JUNQI_PIECE_BLUE_JUNZHANG = 14, // 军长
    JUNQI_PIECE_BLUE_SHIZHANG = 15, // 师长
    JUNQI_PIECE_BLUE_LVZHANG = 16,  // 旅长
    JUNQI_PIECE_BLUE_TUANZHANG = 17,// 团长
    JUNQI_PIECE_BLUE_YINGZHANG = 18,// 营长
    JUNQI_PIECE_BLUE_LIANZHANG = 19,// 连长
    JUNQI_PIECE_BLUE_PAIZHANG = 20, // 排长
    JUNQI_PIECE_BLUE_GONGBING = 21, // 工兵
    JUNQI_PIECE_BLUE_DILEI = 22,    // 地雷
    JUNQI_PIECE_BLUE_ZHADAN = 23,   // 炸弹
    JUNQI_PIECE_BLUE_JUNQI = 24,    // 军旗
} JunqiPiece;

// 玩家
typedef enum {
    JUNQI_PLAYER_RED = 0,
    JUNQI_PLAYER_BLUE = 1
} JunqiPlayer;

// 棋盘位置类型
typedef enum {
    JUNQI_CELL_NORMAL = 0,   // 普通格子
    JUNQI_CELL_CAMP = 1,     // 营（不能吃子，也不能被吃）
    JUNQI_CELL_HEADQUARTERS = 2, // 大本营（军旗只能放这里）
    JUNQI_CELL_RAILWAY = 3,  // 铁路（棋子可以在铁路上走多格）
} JunqiCellType;

// 游戏状态
typedef struct {
    JunqiPiece board[JUNQI_BOARD_HEIGHT][JUNQI_BOARD_WIDTH];
    JunqiPlayer current_player;
    bool game_over;
    JunqiPlayer winner;
    int move_count;
    // 选中的棋子
    int selected_x;
    int selected_y;
    bool has_selected;
    // 上一步的位置
    int last_from_x;
    int last_from_y;
    int last_to_x;
    int last_to_y;
    // 双方司令是否阵亡
    bool red_siling_dead;
    bool blue_siling_dead;
} JunqiGame;

// 初始化游戏
void junqi_game_init(JunqiGame* game);
void junqi_game_reset(JunqiGame* game);

// 检查位置是否有效
bool junqi_is_valid_position(int x, int y);

// 获取棋子所属玩家
JunqiPlayer junqi_get_piece_player(JunqiPiece piece);

// 获取棋子等级（用于吃子判断）
int junqi_get_piece_rank(JunqiPiece piece);

// 检查是否是铁路
bool junqi_is_railway(int x, int y);

// 检查是否是营
bool junqi_is_camp(int x, int y);

// 检查是否是大本营
bool junqi_is_headquarters(int x, int y);

// 检查移动是否合法
bool junqi_is_valid_move(JunqiGame* game, int from_x, int from_y, int to_x, int to_y);

// 执行移动 - 返回是否成功
bool junqi_make_move(JunqiGame* game, int from_x, int from_y, int to_x, int to_y);

// 检查游戏是否结束
bool junqi_check_game_over(JunqiGame* game);

#endif // JUNQI_H
