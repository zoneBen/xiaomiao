#include "chess.h"
#include <string.h>

static void init_board(ChessGame* game) {
    memset(game->board, 0, sizeof(game->board));

    game->board[0][0] = (ChessPiece){PIECE_ROOK, CHESS_COLOR_WHITE, false};
    game->board[0][1] = (ChessPiece){PIECE_KNIGHT, CHESS_COLOR_WHITE, false};
    game->board[0][2] = (ChessPiece){PIECE_BISHOP, CHESS_COLOR_WHITE, false};
    game->board[0][3] = (ChessPiece){PIECE_QUEEN, CHESS_COLOR_WHITE, false};
    game->board[0][4] = (ChessPiece){PIECE_KING, CHESS_COLOR_WHITE, false};
    game->board[0][5] = (ChessPiece){PIECE_BISHOP, CHESS_COLOR_WHITE, false};
    game->board[0][6] = (ChessPiece){PIECE_KNIGHT, CHESS_COLOR_WHITE, false};
    game->board[0][7] = (ChessPiece){PIECE_ROOK, CHESS_COLOR_WHITE, false};
    for (int i = 0; i < 8; i++) {
        game->board[1][i] = (ChessPiece){PIECE_PAWN, CHESS_COLOR_WHITE, false};
    }

    game->board[7][0] = (ChessPiece){PIECE_ROOK, CHESS_COLOR_BLACK, false};
    game->board[7][1] = (ChessPiece){PIECE_KNIGHT, CHESS_COLOR_BLACK, false};
    game->board[7][2] = (ChessPiece){PIECE_BISHOP, CHESS_COLOR_BLACK, false};
    game->board[7][3] = (ChessPiece){PIECE_QUEEN, CHESS_COLOR_BLACK, false};
    game->board[7][4] = (ChessPiece){PIECE_KING, CHESS_COLOR_BLACK, false};
    game->board[7][5] = (ChessPiece){PIECE_BISHOP, CHESS_COLOR_BLACK, false};
    game->board[7][6] = (ChessPiece){PIECE_KNIGHT, CHESS_COLOR_BLACK, false};
    game->board[7][7] = (ChessPiece){PIECE_ROOK, CHESS_COLOR_BLACK, false};
    for (int i = 0; i < 8; i++) {
        game->board[6][i] = (ChessPiece){PIECE_PAWN, CHESS_COLOR_BLACK, false};
    }
}

void chess_game_init(ChessGame* game) {
    chess_game_reset(game);
}

void chess_game_reset(ChessGame* game) {
    memset(game, 0, sizeof(ChessGame));
    init_board(game);
    game->current_turn = CHESS_COLOR_WHITE;
    game->white_castle_kingside = true;
    game->white_castle_queenside = true;
    game->black_castle_kingside = true;
    game->black_castle_queenside = true;
    game->en_passant_x = -1;
    game->en_passant_y = -1;
    game->halfmove_clock = 0;
    game->fullmove_number = 1;
    game->game_over = false;
}

static bool is_in_bounds(int x, int y) {
    return x >= 0 && x < 8 && y >= 0 && y < 8;
}

static bool is_path_clear(ChessGame* game, int from_x, int from_y, int to_x, int to_y) {
    int dx = (to_x > from_x) ? 1 : (to_x < from_x) ? -1 : 0;
    int dy = (to_y > from_y) ? 1 : (to_y < from_y) ? -1 : 0;

    int x = from_x + dx;
    int y = from_y + dy;

    while (x != to_x || y != to_y) {
        if (game->board[x][y].type != PIECE_NONE) {
            return false;
        }
        x += dx;
        y += dy;
    }
    return true;
}

static bool is_valid_pawn_move(ChessGame* game, int from_x, int from_y, int to_x, int to_y) {
    ChessPiece piece = game->board[from_x][from_y];
    ChessPiece target = game->board[to_x][to_y];

    int direction = (piece.color == CHESS_COLOR_WHITE) ? 1 : -1;
    int start_row = (piece.color == CHESS_COLOR_WHITE) ? 1 : 6;

    if (from_y == to_y) {
        if (to_x == from_x + direction && target.type == PIECE_NONE) {
            return true;
        }
        if (from_x == start_row && to_x == from_x + 2 * direction &&
            target.type == PIECE_NONE && game->board[from_x + direction][from_y].type == PIECE_NONE) {
            return true;
        }
    }

    if ((to_y == from_y + 1 || to_y == from_y - 1) && to_x == from_x + direction) {
        if (target.type != PIECE_NONE && target.color != piece.color) {
            return true;
        }
        if (to_x == game->en_passant_x && to_y == game->en_passant_y) {
            return true;
        }
    }

    return false;
}

static bool is_valid_knight_move(int from_x, int from_y, int to_x, int to_y) {
    int dx = to_x - from_x;
    int dy = to_y - from_y;
    return (dx * dx + dy * dy) == 5;
}

static bool is_valid_bishop_move(ChessGame* game, int from_x, int from_y, int to_x, int to_y) {
    int dx = to_x - from_x;
    int dy = to_y - from_y;
    if (dx * dx != dy * dy || dx == 0) {
        return false;
    }
    return is_path_clear(game, from_x, from_y, to_x, to_y);
}

static bool is_valid_rook_move(ChessGame* game, int from_x, int from_y, int to_x, int to_y) {
    if (from_x != to_x && from_y != to_y) {
        return false;
    }
    return is_path_clear(game, from_x, from_y, to_x, to_y);
}

static bool is_valid_queen_move(ChessGame* game, int from_x, int from_y, int to_x, int to_y) {
    return is_valid_rook_move(game, from_x, from_y, to_x, to_y) ||
           is_valid_bishop_move(game, from_x, from_y, to_x, to_y);
}

static bool is_valid_king_move(ChessGame* game, int from_x, int from_y, int to_x, int to_y) {
    int dx = to_x - from_x;
    int dy = to_y - from_y;
    if (dx * dx <= 1 && dy * dy <= 1) {
        return true;
    }

    ChessPiece piece = game->board[from_x][from_y];
    if (piece.type != PIECE_KING || piece.has_moved) {
        return false;
    }

    // Castling: king stays in same row, moves 2 columns
    if (from_x == to_x && abs(dy) == 2) {
        if (piece.color == CHESS_COLOR_WHITE && from_x == 0 && from_y == 4) {
            // White castling
            if (to_y == 6 && game->white_castle_kingside) {
                // Kingside
                return game->board[0][5].type == PIECE_NONE &&
                       game->board[0][6].type == PIECE_NONE;
            }
            if (to_y == 2 && game->white_castle_queenside) {
                // Queenside
                return game->board[0][1].type == PIECE_NONE &&
                       game->board[0][2].type == PIECE_NONE &&
                       game->board[0][3].type == PIECE_NONE;
            }
        }
        if (piece.color == CHESS_COLOR_BLACK && from_x == 7 && from_y == 4) {
            // Black castling
            if (to_y == 6 && game->black_castle_kingside) {
                // Kingside
                return game->board[7][5].type == PIECE_NONE &&
                       game->board[7][6].type == PIECE_NONE;
            }
            if (to_y == 2 && game->black_castle_queenside) {
                // Queenside
                return game->board[7][1].type == PIECE_NONE &&
                       game->board[7][2].type == PIECE_NONE &&
                       game->board[7][3].type == PIECE_NONE;
            }
        }
    }

    return false;
}


bool chess_game_make_move(ChessGame* game, int from_x, int from_y, int to_x, int to_y, ChessPieceType promotion) {
    if (!chess_game_is_valid_move(game, from_x, from_y, to_x, to_y)) {
        return false;
    }

    ChessPiece piece = game->board[from_x][from_y];
    ChessPiece target = game->board[to_x][to_y];

    game->last_move.from_x = from_x;
    game->last_move.from_y = from_y;
    game->last_move.to_x = to_x;
    game->last_move.to_y = to_y;
    game->last_move.captured_piece = target;
    game->last_move.is_castle_kingside = false;
    game->last_move.is_castle_queenside = false;
    game->last_move.is_en_passant = false;
    game->last_move.is_promotion = false;

    if (piece.type == PIECE_PAWN && to_x == game->en_passant_x && to_y == game->en_passant_y) {
        game->last_move.is_en_passant = true;
        int capture_x = (piece.color == CHESS_COLOR_WHITE) ? to_x - 1 : to_x + 1;
        game->board[capture_x][to_y] = (ChessPiece){PIECE_NONE, 0, false};
    }

        if (piece.type == PIECE_KING && abs(to_y - from_y) == 2) {
        if (to_y > from_y) {
            game->last_move.is_castle_kingside = true;
            game->board[to_x][5] = game->board[to_x][7];
            game->board[to_x][7] = (ChessPiece){PIECE_NONE, 0, false};
        } else {
            game->last_move.is_castle_queenside = true;
            game->board[to_x][3] = game->board[to_x][0];
            game->board[to_x][0] = (ChessPiece){PIECE_NONE, 0, false};
        }
    }

    game->board[to_x][to_y] = piece;
    game->board[to_x][to_y].has_moved = true;
    game->board[from_x][from_y] = (ChessPiece){PIECE_NONE, 0, false};

    game->en_passant_x = -1;
    game->en_passant_y = -1;
    if (piece.type == PIECE_PAWN && abs(to_x - from_x) == 2) {
        game->en_passant_x = (from_x + to_x) / 2;
        game->en_passant_y = to_y;
    }

    if (piece.type == PIECE_PAWN && ((piece.color == CHESS_COLOR_WHITE && to_x == 7) || (piece.color == CHESS_COLOR_BLACK && to_x == 0))) {
        game->last_move.is_promotion = true;
        game->last_move.promotion_type = (promotion == PIECE_NONE) ? PIECE_QUEEN : promotion;
        game->board[to_x][to_y].type = game->last_move.promotion_type;
    }

    if (piece.type == PIECE_KING) {
        if (piece.color == CHESS_COLOR_WHITE) {
            game->white_castle_kingside = false;
            game->white_castle_queenside = false;
        } else {
            game->black_castle_kingside = false;
            game->black_castle_queenside = false;
        }
    }
    if (piece.type == PIECE_ROOK) {
        if (piece.color == CHESS_COLOR_WHITE) {
            if (from_x == 0 && from_y == 0) game->white_castle_queenside = false;
            if (from_x == 0 && from_y == 7) game->white_castle_kingside = false;
        } else {
            if (from_x == 7 && from_y == 0) game->black_castle_queenside = false;
            if (from_x == 7 && from_y == 7) game->black_castle_kingside = false;
        }
    }

    if (target.type != PIECE_NONE || piece.type == PIECE_PAWN) {
        game->halfmove_clock = 0;
    } else {
        game->halfmove_clock++;
    }
    if (piece.color == CHESS_COLOR_BLACK) {
        game->fullmove_number++;
    }

    game->current_turn = (game->current_turn == CHESS_COLOR_WHITE) ? CHESS_COLOR_BLACK : CHESS_COLOR_WHITE;

    // Check if the captured piece was the king (only possible if king was left in check)
    if (target.type == PIECE_KING) {
        game->game_over = true;
        game->winner = piece.color; // The player who made the capture wins
    }

    return true;
}

ChessPiece chess_game_get_piece(ChessGame* game, int x, int y) {
    if (!is_in_bounds(x, y)) {
        return (ChessPiece){PIECE_NONE, 0, false};
    }
    return game->board[x][y];
}

ChessColor chess_game_get_current_turn(ChessGame* game) {
    return game->current_turn;
}

bool chess_game_is_game_over(ChessGame* game) {
    return game->game_over;
}

static bool find_king(ChessGame* game, ChessColor color, int* king_x, int* king_y) {
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            if (game->board[x][y].type == PIECE_KING && game->board[x][y].color == color) {
                *king_x = x;
                *king_y = y;
                return true;
            }
        }
    }
    return false;
}

static bool is_square_attacked(ChessGame* game, int x, int y, ChessColor by_color) {
    // Check each square for enemy pieces that can attack this square
    for (int from_x = 0; from_x < 8; from_x++) {
        for (int from_y = 0; from_y < 8; from_y++) {
            ChessPiece piece = game->board[from_x][from_y];
            if (piece.type == PIECE_NONE || piece.color != by_color) {
                continue;
            }

            // Temporarily switch turn to check if the enemy can move to this square
            ChessColor original_turn = game->current_turn;
            game->current_turn = by_color;

            bool can_attack = false;
            switch (piece.type) {
                case PIECE_PAWN: {
                    int direction = (by_color == CHESS_COLOR_WHITE) ? 1 : -1;
                    if (from_x + direction == x && (from_y + 1 == y || from_y - 1 == y)) {
                        can_attack = true;
                    }
                    break;
                }
                case PIECE_KNIGHT:
                    can_attack = is_valid_knight_move(from_x, from_y, x, y);
                    break;
                case PIECE_BISHOP:
                    can_attack = is_valid_bishop_move(game, from_x, from_y, x, y);
                    break;
                case PIECE_ROOK:
                    can_attack = is_valid_rook_move(game, from_x, from_y, x, y);
                    break;
                case PIECE_QUEEN:
                    can_attack = is_valid_queen_move(game, from_x, from_y, x, y);
                    break;
                case PIECE_KING: {
                    int dx = abs(x - from_x);
                    int dy = abs(y - from_y);
                    can_attack = (dx <= 1 && dy <= 1);
                    break;
                }
                default:
                    break;
            }

            game->current_turn = original_turn;

            if (can_attack) {
                return true;
            }
        }
    }
    return false;
}

bool chess_game_is_in_check(ChessGame* game, ChessColor color) {
    int king_x, king_y;
    if (!find_king(game, color, &king_x, &king_y)) {
        return false;
    }
    ChessColor enemy_color = (color == CHESS_COLOR_WHITE) ? CHESS_COLOR_BLACK : CHESS_COLOR_WHITE;
    return is_square_attacked(game, king_x, king_y, enemy_color);
}

static bool move_gets_out_of_check(ChessGame* game, int from_x, int from_y, int to_x, int to_y) {
    // Make a copy of the game state
    ChessGame temp_game = *game;

    // Make the move on the temporary board
    ChessPiece piece = temp_game.board[from_x][from_y];
    temp_game.board[to_x][to_y] = piece;
    temp_game.board[from_x][from_y] = (ChessPiece){PIECE_NONE, 0, false};

    // Check if the king is still in check after this move
    return !chess_game_is_in_check(&temp_game, piece.color);
}

bool chess_game_is_valid_move(ChessGame* game, int from_x, int from_y, int to_x, int to_y) {
    if (!is_in_bounds(from_x, from_y) || !is_in_bounds(to_x, to_y)) {
        return false;
    }

    ChessPiece piece = game->board[from_x][from_y];
    ChessPiece target = game->board[to_x][to_y];

    if (piece.type == PIECE_NONE || piece.color != game->current_turn) {
        return false;
    }

    if (target.type != PIECE_NONE && target.color == piece.color) {
        return false;
    }

    if (from_x == to_x && from_y == to_y) {
        return false;
    }

    bool valid = false;
    switch (piece.type) {
        case PIECE_PAWN: valid = is_valid_pawn_move(game, from_x, from_y, to_x, to_y); break;
        case PIECE_KNIGHT: valid = is_valid_knight_move(from_x, from_y, to_x, to_y); break;
        case PIECE_BISHOP: valid = is_valid_bishop_move(game, from_x, from_y, to_x, to_y); break;
        case PIECE_ROOK: valid = is_valid_rook_move(game, from_x, from_y, to_x, to_y); break;
        case PIECE_QUEEN: valid = is_valid_queen_move(game, from_x, from_y, to_x, to_y); break;
        case PIECE_KING: valid = is_valid_king_move(game, from_x, from_y, to_x, to_y); break;
        default: break;
    }

    // Note: Removed check validation for more entertaining gameplay
    // Players can make moves that leave their king in check,
    // and the opponent can capture the king to win

    return valid;
}

bool chess_game_has_any_valid_moves(ChessGame* game, ChessColor color) {
    ChessColor original_turn = game->current_turn;
    game->current_turn = color;

    bool has_moves = false;
    for (int from_x = 0; from_x < 8 && !has_moves; from_x++) {
        for (int from_y = 0; from_y < 8 && !has_moves; from_y++) {
            ChessPiece piece = game->board[from_x][from_y];
            if (piece.type == PIECE_NONE || piece.color != color) {
                continue;
            }
            for (int to_x = 0; to_x < 8 && !has_moves; to_x++) {
                for (int to_y = 0; to_y < 8 && !has_moves; to_y++) {
                    if (chess_game_is_valid_move(game, from_x, from_y, to_x, to_y)) {
                        has_moves = true;
                    }
                }
            }
        }
    }

    game->current_turn = original_turn;
    return has_moves;
}

bool chess_game_is_checkmate(ChessGame* game, ChessColor color) {
    return chess_game_is_in_check(game, color) && !chess_game_has_any_valid_moves(game, color);
}

bool chess_game_is_stalemate(ChessGame* game, ChessColor color) {
    return !chess_game_is_in_check(game, color) && !chess_game_has_any_valid_moves(game, color);
}

ChessColor chess_game_get_winner(ChessGame* game) {
    return game->winner;
}

bool chess_game_get_valid_moves(ChessGame* game, int x, int y, ChessMove* moves, size_t max_moves) {
    if (!is_in_bounds(x, y)) {
        return false;
    }

    ChessPiece piece = game->board[x][y];
    if (piece.type == PIECE_NONE || piece.color != game->current_turn) {
        return false;
    }

    size_t count = 0;
    for (int to_x = 0; to_x < 8 && count < max_moves; to_x++) {
        for (int to_y = 0; to_y < 8 && count < max_moves; to_y++) {
            if (chess_game_is_valid_move(game, x, y, to_x, to_y)) {
                moves[count].from_x = x;
                moves[count].from_y = y;
                moves[count].to_x = to_x;
                moves[count].to_y = to_y;
                count++;
            }
        }
    }

    return count > 0;
}
