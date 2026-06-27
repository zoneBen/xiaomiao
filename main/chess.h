#ifndef CHESS_H
#define CHESS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CHESS_BOARD_SIZE 8

typedef enum {
    PIECE_NONE = 0,
    PIECE_PAWN,
    PIECE_KNIGHT,
    PIECE_BISHOP,
    PIECE_ROOK,
    PIECE_QUEEN,
    PIECE_KING
} ChessPieceType;

typedef enum {
    COLOR_WHITE = 0,
    COLOR_BLACK
} ChessColor;

typedef struct {
    ChessPieceType type;
    ChessColor color;
    bool has_moved;
} ChessPiece;

typedef struct {
    int8_t from_x;
    int8_t from_y;
    int8_t to_x;
    int8_t to_y;
    ChessPiece captured_piece;
    bool is_castle_kingside;
    bool is_castle_queenside;
    bool is_en_passant;
    bool is_promotion;
    ChessPieceType promotion_type;
} ChessMove;

typedef struct {
    ChessPiece board[CHESS_BOARD_SIZE][CHESS_BOARD_SIZE];
    ChessColor current_turn;
    ChessMove last_move;
    bool white_castle_kingside;
    bool white_castle_queenside;
    bool black_castle_kingside;
    bool black_castle_queenside;
    int8_t en_passant_x;
    int8_t en_passant_y;
    int halfmove_clock;
    int fullmove_number;
    bool game_over;
    ChessColor winner;
} ChessGame;

void chess_game_init(ChessGame* game);
void chess_game_reset(ChessGame* game);
bool chess_game_is_valid_move(ChessGame* game, int from_x, int from_y, int to_x, int to_y);
bool chess_game_make_move(ChessGame* game, int from_x, int from_y, int to_x, int to_y, ChessPieceType promotion);
ChessPiece chess_game_get_piece(ChessGame* game, int x, int y);
ChessColor chess_game_get_current_turn(ChessGame* game);
bool chess_game_is_game_over(ChessGame* game);
bool chess_game_get_valid_moves(ChessGame* game, int x, int y, ChessMove* moves, size_t max_moves);

#endif
