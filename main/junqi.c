#include "junqi.h"
#include <string.h>
#include "esp_log.h"

static const char* TAG = "JunQi";

// 检查位置是否有效
bool junqi_is_valid_position(int x, int y)
{
    return x >= 0 && x < JUNQI_BOARD_WIDTH && y >= 0 && y < JUNQI_BOARD_HEIGHT;
}

// 获取棋子所属玩家
JunqiPlayer junqi_get_piece_player(JunqiPiece piece)
{
    if (piece >= JUNQI_PIECE_RED_SILING && piece <= JUNQI_PIECE_RED_JUNQI) {
        return JUNQI_PLAYER_RED;
    } else if (piece >= JUNQI_PIECE_BLUE_SILING && piece <= JUNQI_PIECE_BLUE_JUNQI) {
        return JUNQI_PLAYER_BLUE;
    }
    return JUNQI_PLAYER_RED;
}

// 获取棋子等级（用于吃子判断）
int junqi_get_piece_rank(JunqiPiece piece)
{
    // 司令 = 12, 军长 = 11, ..., 工兵 = 4, 地雷 = 3, 炸弹 = 2, 军旗 = 1
    int rank = 0;
    if (piece >= JUNQI_PIECE_RED_SILING && piece <= JUNQI_PIECE_RED_JUNQI) {
        rank = 13 - (piece - JUNQI_PIECE_RED_SILING);
    } else if (piece >= JUNQI_PIECE_BLUE_SILING && piece <= JUNQI_PIECE_BLUE_JUNQI) {
        rank = 13 - (piece - JUNQI_PIECE_BLUE_SILING);
    }
    return rank;
}

// 检查是否是铁路
bool junqi_is_railway(int x, int y)
{
    // 铁路：第一行、中间行、最后一行，以及左右两列
    if (y == 0 || y == 4 || y == 8) return true;
    if (x == 0 || x == 6) return true;
    return false;
}

// 检查是否是营
bool junqi_is_camp(int x, int y)
{
    int camps[6][2] = {
        {1, 2}, {3, 1}, {5, 2},
        {1, 6}, {3, 7}, {5, 6}
    };
    for (int i = 0; i < 6; i++) {
        if (camps[i][0] == x && camps[i][1] == y) {
            return true;
        }
    }
    return false;
}

// 检查是否是大本营
bool junqi_is_headquarters(int x, int y)
{
    if ((y == 0 && (x == 2 || x == 4)) || (y == 8 && (x == 2 || x == 4))) {
        return true;
    }
    return false;
}

// 检查移动是否合法
bool junqi_is_valid_move(JunqiGame* game, int from_x, int from_y, int to_x, int to_y)
{
    if (!junqi_is_valid_position(from_x, from_y) || !junqi_is_valid_position(to_x, to_y)) {
        return false;
    }

    JunqiPiece piece = game->board[from_y][from_x];
    if (piece == JUNQI_PIECE_NONE) {
        return false;
    }

    if (junqi_get_piece_player(piece) != game->current_player) {
        return false;
    }

    // 不能移动地雷和军旗
    if (piece == JUNQI_PIECE_RED_DILEI || piece == JUNQI_PIECE_RED_JUNQI ||
        piece == JUNQI_PIECE_BLUE_DILEI || piece == JUNQI_PIECE_BLUE_JUNQI) {
        return false;
    }

    JunqiPiece target = game->board[to_y][to_x];
    if (target != JUNQI_PIECE_NONE) {
        if (junqi_get_piece_player(target) == game->current_player) {
            return false;
        }
    }

    // 检查是否是营，营里不能被吃
    if (junqi_is_camp(to_x, to_y) && target != JUNQI_PIECE_NONE) {
        return false;
    }

    // 计算距离
    int dx = (to_x > from_x) ? (to_x - from_x) : (from_x - to_x);
    int dy = (to_y > from_y) ? (to_y - from_y) : (from_y - to_y);

    // 普通移动：上下左右一格
    bool normal_move = ((dx == 1 && dy == 0) || (dx == 0 && dy == 1));

    // 铁路移动
    bool railway_move = false;
    if (junqi_is_railway(from_x, from_y) && junqi_is_railway(to_x, to_y)) {
        // 工兵可以在铁路上转弯，其他棋子只能直线
        bool is_gongbing = (piece == JUNQI_PIECE_RED_GONGBING || piece == JUNQI_PIECE_BLUE_GONGBING);

        if (is_gongbing) {
            // 工兵：只要在铁路上就能移动（简化处理）
            railway_move = true;
        } else {
            // 其他棋子：只能直线移动
            if ((from_x == to_x) || (from_y == to_y)) {
                // 检查路径上是否有障碍物
                bool blocked = false;
                if (from_x == to_x) {
                    // 垂直移动
                    int min_y = (from_y < to_y) ? from_y : to_y;
                    int max_y = (from_y > to_y) ? from_y : to_y;
                    for (int y = min_y + 1; y < max_y; y++) {
                        if (game->board[y][from_x] != JUNQI_PIECE_NONE) {
                            blocked = true;
                            break;
                        }
                    }
                } else {
                    // 水平移动
                    int min_x = (from_x < to_x) ? from_x : to_x;
                    int max_x = (from_x > to_x) ? from_x : to_x;
                    for (int x = min_x + 1; x < max_x; x++) {
                        if (game->board[from_y][x] != JUNQI_PIECE_NONE) {
                            blocked = true;
                            break;
                        }
                    }
                }
                if (!blocked) {
                    railway_move = true;
                }
            }
        }
    }

    if (!normal_move && !railway_move) {
        return false;
    }

    return true;
}

// 执行移动
bool junqi_make_move(JunqiGame* game, int from_x, int from_y, int to_x, int to_y)
{
    if (!junqi_is_valid_move(game, from_x, from_y, to_x, to_y)) {
        return false;
    }

    JunqiPiece piece = game->board[from_y][from_x];
    JunqiPiece target = game->board[to_y][to_x];

    // 处理吃子
    if (target != JUNQI_PIECE_NONE) {
        bool is_gongbing = (piece == JUNQI_PIECE_RED_GONGBING || piece == JUNQI_PIECE_BLUE_GONGBING);
        bool is_zhadan = (piece == JUNQI_PIECE_RED_ZHADAN || piece == JUNQI_PIECE_BLUE_ZHADAN);
        bool target_is_dilei = (target == JUNQI_PIECE_RED_DILEI || target == JUNQI_PIECE_BLUE_DILEI);
        bool target_is_zhadan = (target == JUNQI_PIECE_RED_ZHADAN || target == JUNQI_PIECE_BLUE_ZHADAN);
        bool target_is_junqi = (target == JUNQI_PIECE_RED_JUNQI || target == JUNQI_PIECE_BLUE_JUNQI);

        // 检查司令阵亡
        if (target == JUNQI_PIECE_RED_SILING) {
            game->red_siling_dead = true;
        } else if (target == JUNQI_PIECE_BLUE_SILING) {
            game->blue_siling_dead = true;
        }

        // 特殊吃子规则
        if (is_zhadan || target_is_zhadan) {
            // 炸弹同归于尽
            game->board[from_y][from_x] = JUNQI_PIECE_NONE;
            game->board[to_y][to_x] = JUNQI_PIECE_NONE;
        } else if (is_gongbing && target_is_dilei) {
            // 工兵挖地雷
            game->board[from_y][from_x] = JUNQI_PIECE_NONE;
            game->board[to_y][to_x] = piece;
        } else if (target_is_dilei) {
            // 其他棋子踩地雷同归于尽
            game->board[from_y][from_x] = JUNQI_PIECE_NONE;
            game->board[to_y][to_x] = JUNQI_PIECE_NONE;
        } else if (target_is_junqi) {
            // 扛军旗，获胜
            game->board[from_y][from_x] = JUNQI_PIECE_NONE;
            game->board[to_y][to_x] = piece;
            game->game_over = true;
            game->winner = game->current_player;
        } else {
            // 普通吃子：大吃小
            int piece_rank = junqi_get_piece_rank(piece);
            int target_rank = junqi_get_piece_rank(target);
            if (piece_rank >= target_rank) {
                game->board[from_y][from_x] = JUNQI_PIECE_NONE;
                game->board[to_y][to_x] = piece;
            } else {
                // 小吃大，同归于尽
                game->board[from_y][from_x] = JUNQI_PIECE_NONE;
                game->board[to_y][to_x] = JUNQI_PIECE_NONE;
            }
        }
    } else {
        // 普通移动
        game->board[from_y][from_x] = JUNQI_PIECE_NONE;
        game->board[to_y][to_x] = piece;
    }

    // 保存上一步
    game->last_from_x = from_x;
    game->last_from_y = from_y;
    game->last_to_x = to_x;
    game->last_to_y = to_y;

    // 切换玩家
    game->current_player = (game->current_player == JUNQI_PLAYER_RED) ?
                            JUNQI_PLAYER_BLUE : JUNQI_PLAYER_RED;
    game->move_count++;

    // 检查游戏是否结束
    junqi_check_game_over(game);

    return true;
}

// 检查游戏是否结束
bool junqi_check_game_over(JunqiGame* game)
{
    if (game->game_over) {
        return true;
    }

    // 检查一方是否还有可移动棋子
    bool red_has_movable = false;
    bool blue_has_movable = false;

    for (int y = 0; y < JUNQI_BOARD_HEIGHT; y++) {
        for (int x = 0; x < JUNQI_BOARD_WIDTH; x++) {
            JunqiPiece piece = game->board[y][x];
            if (piece == JUNQI_PIECE_NONE) continue;

            // 跳过地雷和军旗
            if (piece == JUNQI_PIECE_RED_DILEI || piece == JUNQI_PIECE_RED_JUNQI ||
                piece == JUNQI_PIECE_BLUE_DILEI || piece == JUNQI_PIECE_BLUE_JUNQI) {
                continue;
            }

            if (junqi_get_piece_player(piece) == JUNQI_PLAYER_RED) {
                red_has_movable = true;
            } else {
                blue_has_movable = true;
            }
        }
    }

    if (!red_has_movable) {
        game->game_over = true;
        game->winner = JUNQI_PLAYER_BLUE;
        return true;
    }
    if (!blue_has_movable) {
        game->game_over = true;
        game->winner = JUNQI_PLAYER_RED;
        return true;
    }

    return false;
}

// 初始化游戏
void junqi_game_init(JunqiGame* game)
{
    junqi_game_reset(game);
}

void junqi_game_reset(JunqiGame* game)
{
    memset(game, 0, sizeof(JunqiGame));
    game->current_player = JUNQI_PLAYER_RED;
    game->game_over = false;
    game->winner = JUNQI_PLAYER_RED;
    game->move_count = 0;
    game->has_selected = false;
    game->last_from_x = -1;
    game->last_from_y = -1;
    game->last_to_x = -1;
    game->last_to_y = -1;
    game->red_siling_dead = false;
    game->blue_siling_dead = false;

    // 简化布局：红方在上，蓝方在下
    // 红方 (上半部分)
    game->board[0][3] = JUNQI_PIECE_RED_JUNQI;       // 军旗（大本营）
    game->board[1][0] = JUNQI_PIECE_RED_GONGBING;    // 工兵
    game->board[1][3] = JUNQI_PIECE_RED_SILING;      // 司令
    game->board[1][6] = JUNQI_PIECE_RED_GONGBING;    // 工兵
    game->board[2][1] = JUNQI_PIECE_RED_JUNZHANG;    // 军长
    game->board[2][5] = JUNQI_PIECE_RED_SHIZHANG;    // 师长
    game->board[3][0] = JUNQI_PIECE_RED_DILEI;       // 地雷
    game->board[3][2] = JUNQI_PIECE_RED_LVZHANG;     // 旅长
    game->board[3][4] = JUNQI_PIECE_RED_TUANZHANG;   // 团长
    game->board[3][6] = JUNQI_PIECE_RED_DILEI;       // 地雷
    game->board[4][1] = JUNQI_PIECE_RED_YINGZHANG;   // 营长
    game->board[4][3] = JUNQI_PIECE_RED_ZHADAN;      // 炸弹
    game->board[4][5] = JUNQI_PIECE_RED_LIANZHANG;   // 连长

    // 蓝方 (下半部分)
    game->board[8][3] = JUNQI_PIECE_BLUE_JUNQI;       // 军旗（大本营）
    game->board[7][0] = JUNQI_PIECE_BLUE_GONGBING;    // 工兵
    game->board[7][3] = JUNQI_PIECE_BLUE_SILING;      // 司令
    game->board[7][6] = JUNQI_PIECE_BLUE_GONGBING;    // 工兵
    game->board[6][1] = JUNQI_PIECE_BLUE_JUNZHANG;    // 军长
    game->board[6][5] = JUNQI_PIECE_BLUE_SHIZHANG;    // 师长
    game->board[5][0] = JUNQI_PIECE_BLUE_DILEI;       // 地雷
    game->board[5][2] = JUNQI_PIECE_BLUE_LVZHANG;     // 旅长
    game->board[5][4] = JUNQI_PIECE_BLUE_TUANZHANG;   // 团长
    game->board[5][6] = JUNQI_PIECE_BLUE_DILEI;       // 地雷
    game->board[4][0] = JUNQI_PIECE_BLUE_PAIZHANG;    // 排长
    game->board[4][6] = JUNQI_PIECE_BLUE_PAIZHANG;    // 排长
    game->board[0][2] = JUNQI_PIECE_RED_PAIZHANG;     // 排长（红方）
    game->board[0][4] = JUNQI_PIECE_RED_SHIZHANG;     // 师长（红方，补充）
    game->board[8][2] = JUNQI_PIECE_BLUE_PAIZHANG;    // 排长（蓝方）
    game->board[8][4] = JUNQI_PIECE_BLUE_SHIZHANG;    // 师长（蓝方，补充）
}
