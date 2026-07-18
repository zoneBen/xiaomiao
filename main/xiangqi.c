#include "xiangqi.h"
#include <string.h>

// ==================== 辅助函数 ====================

static bool is_in_board(int x, int y)
{
    return x >= 0 && x < XIANGQI_BOARD_WIDTH && y >= 0 && y < XIANGQI_BOARD_HEIGHT;
}

// 检查棋子是否属于某方
bool xiangqi_is_red_piece(XiangqiPieceType type)
{
    return type >= XIANGQI_PIECE_KING && type <= XIANGQI_PIECE_PAWN;
}

bool xiangqi_is_black_piece(XiangqiPieceType type)
{
    return type >= XIANGQI_PIECE_KING_B && type <= XIANGQI_PIECE_PAWN_B;
}

XiangqiColor xiangqi_get_piece_color(XiangqiPiece piece)
{
    if (xiangqi_is_red_piece(piece.type)) return XIANGQI_COLOR_RED;
    if (xiangqi_is_black_piece(piece.type)) return XIANGQI_COLOR_BLACK;
    return XIANGQI_COLOR_NONE;
}

// 检查坐标是否在九宫内（将/帅/士活动范围）
static bool is_in_palace(int x, int y, XiangqiColor color)
{
    if (x < 3 || x > 5) return false;
    if (color == XIANGQI_COLOR_RED) {
        return y >= 7 && y <= 9;
    } else {
        return y >= 0 && y <= 2;
    }
}

// 检查是否过河（兵/卒、象）
static bool has_crossed_river(int y, XiangqiColor color)
{
    if (color == XIANGQI_COLOR_RED) {
        return y <= 4; // 红方过河是 y <=4
    } else {
        return y >= 5; // 黑方过河是 y >=5
    }
}

// 检查两点之间是否有棋子（直线，用于车/炮）
static int count_pieces_between(XiangqiGame* game, int x1, int y1, int x2, int y2)
{
    int count = 0;
    int dx = (x2 > x1) ? 1 : (x2 < x1) ? -1 : 0;
    int dy = (y2 > y1) ? 1 : (y2 < y1) ? -1 : 0;

    int x = x1 + dx;
    int y = y1 + dy;

    while (x != x2 || y != y2) {
        if (game->board[y][x].type != XIANGQI_PIECE_NONE) {
            count++;
        }
        x += dx;
        y += dy;
    }

    return count;
}

// ==================== 棋盘初始化 ====================

static void xiangqi_init_board(XiangqiGame* game)
{
    // 清空棋盘
    memset(game->board, 0, sizeof(game->board));

    // 黑方棋子（上方）
    game->board[0][0].type = XIANGQI_PIECE_CHARIOT_B;
    game->board[0][1].type = XIANGQI_PIECE_HORSE_B;
    game->board[0][2].type = XIANGQI_PIECE_ELEPHANT_B;
    game->board[0][3].type = XIANGQI_PIECE_ADVISOR_B;
    game->board[0][4].type = XIANGQI_PIECE_KING_B;
    game->board[0][5].type = XIANGQI_PIECE_ADVISOR_B;
    game->board[0][6].type = XIANGQI_PIECE_ELEPHANT_B;
    game->board[0][7].type = XIANGQI_PIECE_HORSE_B;
    game->board[0][8].type = XIANGQI_PIECE_CHARIOT_B;

    game->board[2][1].type = XIANGQI_PIECE_CANNON_B;
    game->board[2][7].type = XIANGQI_PIECE_CANNON_B;

    game->board[3][0].type = XIANGQI_PIECE_PAWN_B;
    game->board[3][2].type = XIANGQI_PIECE_PAWN_B;
    game->board[3][4].type = XIANGQI_PIECE_PAWN_B;
    game->board[3][6].type = XIANGQI_PIECE_PAWN_B;
    game->board[3][8].type = XIANGQI_PIECE_PAWN_B;

    // 红方棋子（下方）
    game->board[9][0].type = XIANGQI_PIECE_CHARIOT;
    game->board[9][1].type = XIANGQI_PIECE_HORSE;
    game->board[9][2].type = XIANGQI_PIECE_ELEPHANT;
    game->board[9][3].type = XIANGQI_PIECE_ADVISOR;
    game->board[9][4].type = XIANGQI_PIECE_KING;
    game->board[9][5].type = XIANGQI_PIECE_ADVISOR;
    game->board[9][6].type = XIANGQI_PIECE_ELEPHANT;
    game->board[9][7].type = XIANGQI_PIECE_HORSE;
    game->board[9][8].type = XIANGQI_PIECE_CHARIOT;

    game->board[7][1].type = XIANGQI_PIECE_CANNON;
    game->board[7][7].type = XIANGQI_PIECE_CANNON;

    game->board[6][0].type = XIANGQI_PIECE_PAWN;
    game->board[6][2].type = XIANGQI_PIECE_PAWN;
    game->board[6][4].type = XIANGQI_PIECE_PAWN;
    game->board[6][6].type = XIANGQI_PIECE_PAWN;
    game->board[6][8].type = XIANGQI_PIECE_PAWN;
}

void xiangqi_game_init(XiangqiGame* game)
{
    xiangqi_game_reset(game);
}

void xiangqi_game_reset(XiangqiGame* game)
{
    memset(game, 0, sizeof(XiangqiGame));
    xiangqi_init_board(game);
    game->current_turn = XIANGQI_COLOR_RED;
    game->game_over = false;
    game->winner = XIANGQI_COLOR_NONE;
    game->move_count = 0;
}

// ==================== 棋子访问 ====================

XiangqiPiece xiangqi_get_piece(XiangqiGame* game, int x, int y)
{
    if (!is_in_board(x, y)) {
        XiangqiPiece empty = {XIANGQI_PIECE_NONE, XIANGQI_COLOR_NONE};
        return empty;
    }
    return game->board[y][x];
}

// ==================== 将/帅移动验证 ====================

static bool is_valid_king_move(XiangqiGame* game, int from_x, int from_y, int to_x, int to_y, XiangqiColor color)
{
    // 必须在九宫内
    if (!is_in_palace(to_x, to_y, color)) return false;

    // 每次移动一格（横或竖）
    int dx = (to_x > from_x) ? (to_x - from_x) : (from_x - to_x);
    int dy = (to_y > from_y) ? (to_y - from_y) : (from_y - to_y);
    if (!((dx == 1 && dy == 0) || (dx == 0 && dy == 1))) {
        // 检查是否是飞将（将帅对面）
        if (dx == 0 && dy != 0) {
            // 同列，检查中间是否有子
            if (count_pieces_between(game, from_x, from_y, to_x, to_y) == 0) {
                // 看目标位置是不是对方的将
                XiangqiPiece target = game->board[to_y][to_x];
                if (target.type != XIANGQI_PIECE_NONE) {
                    XiangqiColor target_color = xiangqi_get_piece_color(target);
                    if (target_color != color) {
                        // 检查目标是不是将/帅
                        if ((color == XIANGQI_COLOR_RED && target.type == XIANGQI_PIECE_KING_B) ||
                            (color == XIANGQI_COLOR_BLACK && target.type == XIANGQI_PIECE_KING)) {
                            return true; // 可以飞将吃
                        }
                    }
                }
            }
        }
        return false;
    }
    return true;
}

// ==================== 士移动验证 ====================

static bool is_valid_advisor_move(int from_x, int from_y, int to_x, int to_y, XiangqiColor color)
{
    // 必须在九宫内
    if (!is_in_palace(to_x, to_y, color)) return false;

    // 斜走一格
    int dx = (to_x > from_x) ? (to_x - from_x) : (from_x - to_x);
    int dy = (to_y > from_y) ? (to_y - from_y) : (from_y - to_y);
    return (dx == 1 && dy == 1);
}

// ==================== 象移动验证 ====================

static bool is_valid_elephant_move(XiangqiGame* game, int from_x, int from_y, int to_x, int to_y, XiangqiColor color)
{
    // 不能过河
    if (has_crossed_river(to_y, color)) return false;

    // 走田字
    int dx = (to_x > from_x) ? (to_x - from_x) : (from_x - to_x);
    int dy = (to_y > from_y) ? (to_y - from_y) : (from_y - to_y);
    if (dx != 2 || dy != 2) return false;

    // 检查象眼是否被堵
    int eye_x = from_x + (to_x - from_x) / 2;
    int eye_y = from_y + (to_y - from_y) / 2;
    if (game->board[eye_y][eye_x].type != XIANGQI_PIECE_NONE) return false;

    return true;
}

// ==================== 马移动验证 ====================

static bool is_valid_horse_move(XiangqiGame* game, int from_x, int from_y, int to_x, int to_y)
{
    // 走马走日字
    int dx = (to_x > from_x) ? (to_x - from_x) : (from_x - to_x);
    int dy = (to_y > from_y) ? (to_y - from_y) : (from_y - to_y);
    if (!((dx == 2 && dy == 1) || (dx == 1 && dy == 2))) return false;

    // 检查马腿是否被别
    int leg_x = from_x, leg_y = from_y;
    if (dx == 2) {
        leg_x = from_x + (to_x - from_x) / 2;
    } else {
        leg_y = from_y + (to_y - from_y) / 2;
    }
    if (game->board[leg_y][leg_x].type != XIANGQI_PIECE_NONE) return false;

    return true;
}

// ==================== 车移动验证 ====================

static bool is_valid_chariot_move(XiangqiGame* game, int from_x, int from_y, int to_x, int to_y)
{
    // 走直线
    if (from_x != to_x && from_y != to_y) return false;

    // 中间不能有子
    if (count_pieces_between(game, from_x, from_y, to_x, to_y) != 0) return false;

    return true;
}

// ==================== 炮移动验证 ====================

static bool is_valid_cannon_move(XiangqiGame* game, int from_x, int from_y, int to_x, int to_y)
{
    // 走直线
    if (from_x != to_x && from_y != to_y) return false;

    int between = count_pieces_between(game, from_x, from_y, to_x, to_y);
    XiangqiPiece target = game->board[to_y][to_x];

    if (target.type == XIANGQI_PIECE_NONE) {
        // 移动：中间不能有子
        return between == 0;
    } else {
        // 吃子：中间必须正好有一个子（炮架）
        return between == 1;
    }
}

// ==================== 兵/卒移动验证 ====================

static bool is_valid_pawn_move(XiangqiGame* game, int from_x, int from_y, int to_x, int to_y, XiangqiColor color)
{
    int dx = to_x - from_x;
    int dy = to_y - from_y;

    // 红兵向前是 y 减小，黑卒向前是 y 增大
    int forward_dir = (color == XIANGQI_COLOR_RED) ? -1 : 1;

    if (dx == 0 && dy == forward_dir) {
        // 向前走一格
        return true;
    }

    // 过河后可以横走
    if (has_crossed_river(from_y, color)) {
        if ((dx == 1 || dx == -1) && dy == 0) {
            return true;
        }
    }

    return false;
}

// ==================== 通用移动验证 ====================

bool xiangqi_is_valid_move(XiangqiGame* game, int from_x, int from_y, int to_x, int to_y)
{
    // 边界检查
    if (!is_in_board(from_x, from_y) || !is_in_board(to_x, to_y)) {
        return false;
    }

    XiangqiPiece piece = game->board[from_y][from_x];
    if (piece.type == XIANGQI_PIECE_NONE) return false;

    XiangqiColor color = xiangqi_get_piece_color(piece);
    XiangqiPiece target = game->board[to_y][to_x];

    // 不能吃自己的棋子
    if (target.type != XIANGQI_PIECE_NONE) {
        XiangqiColor target_color = xiangqi_get_piece_color(target);
        if (target_color == color) return false;
    }

    // 根据棋子类型验证移动
    bool valid = false;
    switch (piece.type) {
        case XIANGQI_PIECE_KING:
        case XIANGQI_PIECE_KING_B:
            valid = is_valid_king_move(game, from_x, from_y, to_x, to_y, color);
            break;

        case XIANGQI_PIECE_ADVISOR:
        case XIANGQI_PIECE_ADVISOR_B:
            valid = is_valid_advisor_move(from_x, from_y, to_x, to_y, color);
            break;

        case XIANGQI_PIECE_ELEPHANT:
        case XIANGQI_PIECE_ELEPHANT_B:
            valid = is_valid_elephant_move(game, from_x, from_y, to_x, to_y, color);
            break;

        case XIANGQI_PIECE_HORSE:
        case XIANGQI_PIECE_HORSE_B:
            valid = is_valid_horse_move(game, from_x, from_y, to_x, to_y);
            break;

        case XIANGQI_PIECE_CHARIOT:
        case XIANGQI_PIECE_CHARIOT_B:
            valid = is_valid_chariot_move(game, from_x, from_y, to_x, to_y);
            break;

        case XIANGQI_PIECE_CANNON:
        case XIANGQI_PIECE_CANNON_B:
            valid = is_valid_cannon_move(game, from_x, from_y, to_x, to_y);
            break;

        case XIANGQI_PIECE_PAWN:
        case XIANGQI_PIECE_PAWN_B:
            valid = is_valid_pawn_move(game, from_x, from_y, to_x, to_y, color);
            break;

        default:
            break;
    }

    if (!valid) return false;

    // 模拟移动，检查是否会导致被将
    XiangqiGame temp = *game;
    temp.board[to_y][to_x] = temp.board[from_y][from_x];
    temp.board[from_y][from_x].type = XIANGQI_PIECE_NONE;
    if (xiangqi_is_in_check(&temp, color)) {
        return false;
    }

    return true;
}

// ==================== 移动执行 ====================

bool xiangqi_make_move(XiangqiGame* game, int from_x, int from_y, int to_x, int to_y)
{
    if (!xiangqi_is_valid_move(game, from_x, from_y, to_x, to_y)) {
        return false;
    }

    XiangqiPiece target = game->board[to_y][to_x];
    XiangqiPiece moving = game->board[from_y][from_x];

    // 执行移动
    game->board[to_y][to_x] = moving;
    game->board[from_y][from_x].type = XIANGQI_PIECE_NONE;

    // 检查是否吃了对方的将/帅
    if (target.type == XIANGQI_PIECE_KING || target.type == XIANGQI_PIECE_KING_B) {
        game->game_over = true;
        game->winner = xiangqi_get_piece_color(moving);
    } else {
        // 切换回合
        game->current_turn = (game->current_turn == XIANGQI_COLOR_RED)
                            ? XIANGQI_COLOR_BLACK
                            : XIANGQI_COLOR_RED;
        game->move_count++;
    }

    return true;
}

// ==================== 将军检测 ====================

// 查找某方的将/帅位置
static bool find_king_position(XiangqiGame* game, XiangqiColor color, int* out_x, int* out_y)
{
    XiangqiPieceType king_type = (color == XIANGQI_COLOR_RED) ? XIANGQI_PIECE_KING : XIANGQI_PIECE_KING_B;
    for (int y = 0; y < XIANGQI_BOARD_HEIGHT; y++) {
        for (int x = 0; x < XIANGQI_BOARD_WIDTH; x++) {
            if (game->board[y][x].type == king_type) {
                *out_x = x;
                *out_y = y;
                return true;
            }
        }
    }
    return false;
}

bool xiangqi_is_in_check(XiangqiGame* game, XiangqiColor color)
{
    int king_x, king_y;
    if (!find_king_position(game, color, &king_x, &king_y)) {
        return true; // 将/帅不在了，等于被将死
    }

    // 检查对方所有棋子是否能攻击到将/帅
    XiangqiColor enemy = (color == XIANGQI_COLOR_RED) ? XIANGQI_COLOR_BLACK : XIANGQI_COLOR_RED;

    for (int y = 0; y < XIANGQI_BOARD_HEIGHT; y++) {
        for (int x = 0; x < XIANGQI_BOARD_WIDTH; x++) {
            XiangqiPiece p = game->board[y][x];
            if (p.type != XIANGQI_PIECE_NONE && xiangqi_get_piece_color(p) == enemy) {
                // 我们不能直接调用 xiangqi_is_valid_move，因为吃将是特殊情况
                // 所以我们手动验证每一种棋子是否能攻击到将/帅
                bool can_capture = false;

                switch (p.type) {
                    case XIANGQI_PIECE_KING:
                    case XIANGQI_PIECE_KING_B:
                        can_capture = is_valid_king_move(game, x, y, king_x, king_y, enemy);
                        break;
                    case XIANGQI_PIECE_ADVISOR:
                    case XIANGQI_PIECE_ADVISOR_B:
                        can_capture = is_valid_advisor_move(x, y, king_x, king_y, enemy);
                        break;
                    case XIANGQI_PIECE_ELEPHANT:
                    case XIANGQI_PIECE_ELEPHANT_B:
                        can_capture = is_valid_elephant_move(game, x, y, king_x, king_y, enemy);
                        break;
                    case XIANGQI_PIECE_HORSE:
                    case XIANGQI_PIECE_HORSE_B:
                        can_capture = is_valid_horse_move(game, x, y, king_x, king_y);
                        break;
                    case XIANGQI_PIECE_CHARIOT:
                    case XIANGQI_PIECE_CHARIOT_B:
                        can_capture = is_valid_chariot_move(game, x, y, king_x, king_y);
                        break;
                    case XIANGQI_PIECE_CANNON:
                    case XIANGQI_PIECE_CANNON_B:
                        // 炮吃子要检查炮架
                        if (x == king_x || y == king_y) {
                            if (count_pieces_between(game, x, y, king_x, king_y) == 1) {
                                can_capture = true;
                            }
                        }
                        break;
                    case XIANGQI_PIECE_PAWN:
                    case XIANGQI_PIECE_PAWN_B:
                        can_capture = is_valid_pawn_move(game, x, y, king_x, king_y, enemy);
                        break;
                    default:
                        break;
                }

                if (can_capture) {
                    return true;
                }
            }
        }
    }

    return false;
}

// ==================== 将死检测 ====================

bool xiangqi_is_checkmate(XiangqiGame* game, XiangqiColor color)
{
    if (!xiangqi_is_in_check(game, color)) {
        return false;
    }

    // 检查是否有任何合法移动可以解除将军
    for (int from_y = 0; from_y < XIANGQI_BOARD_HEIGHT; from_y++) {
        for (int from_x = 0; from_x < XIANGQI_BOARD_WIDTH; from_x++) {
            XiangqiPiece p = game->board[from_y][from_x];
            if (p.type == XIANGQI_PIECE_NONE) continue;
            if (xiangqi_get_piece_color(p) != color) continue;

            // 尝试所有可能的移动
            for (int to_y = 0; to_y < XIANGQI_BOARD_HEIGHT; to_y++) {
                for (int to_x = 0; to_x < XIANGQI_BOARD_WIDTH; to_x++) {
                    if (from_x == to_x && from_y == to_y) continue;

                    if (xiangqi_is_valid_move(game, from_x, from_y, to_x, to_y)) {
                        // 有合法移动，不是将死
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

// ==================== 游戏状态 ====================

XiangqiColor xiangqi_get_current_turn(XiangqiGame* game)
{
    return game->current_turn;
}

bool xiangqi_is_game_over(XiangqiGame* game)
{
    return game->game_over;
}

XiangqiColor xiangqi_get_winner(XiangqiGame* game)
{
    return game->winner;
}
