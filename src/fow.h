/* mist-c: Fog of War chess rules (milestone M1).
 *
 * Rules as in Zhang & Sandholm (Obscuro), Appendix A: the king is captured,
 * there is no check, castling ignores attacks, and each side observes the
 * squares its pieces can legally move to. Squares are 0..63 = a1..h8.
 */
#ifndef FOW_H
#define FOW_H

#include <stdint.h>

typedef uint64_t bb_t;

enum { WHITE = 0, BLACK = 1 };
enum { PAWN = 0, KNIGHT, BISHOP, ROOK, QUEEN, KING, NO_PIECE };

typedef struct {
    bb_t by_color[2];
    bb_t by_type[6];
    int8_t piece[64];   /* piece type on the square, NO_PIECE if empty */
    int8_t color[64];   /* color on the square, -1 if empty */
    int side;           /* side to move */
    bb_t castle_rooks;  /* rooks that may still castle (bitmask of their squares) */
    int ep;             /* en-passant target square, -1 if none */
    int halfmove;
} Position;

/* A move: from | to << 6 | promo << 12 (promo: NO_PIECE or KNIGHT..QUEEN). */
typedef uint16_t Move;
#define MOVE_FROM(m) ((m) & 63)
#define MOVE_TO(m) (((m) >> 6) & 63)
#define MOVE_PROMO(m) (((m) >> 12) & 7)
#define MAKE_MOVE(f, t, p) ((Move)((f) | ((t) << 6) | ((p) << 12)))

typedef struct { Move moves[256]; int count; } MoveList;

typedef struct {
    int captured_type, captured_color, captured_square;
    bb_t castle_rooks;
    int ep, halfmove;
    int rook_from, rook_to;  /* for castling; -1 otherwise */
} Undo;

void fow_init(void);
void pos_clear(Position *p);
void pos_start(Position *p);
void pos_put(Position *p, int square, int color, int type);
void pos_remove(Position *p, int square);
/* 64-char board string (a1..h8, letters, '-' empty), side, castling rook mask, ep square. */
int pos_from_board_string(Position *p, const char *text, int side, bb_t castle_rooks, int ep);
void pos_to_board_string(const Position *p, char out[65]);

bb_t attacks_from(const Position *p, int square);  /* squares the piece on `square` attacks */
int is_attacked(const Position *p, int square, int by_color);
void generate_moves(const Position *p, MoveList *list);  /* Fog of War legal moves */
void make_move(Position *p, Move m, Undo *u);
void unmake_move(Position *p, Move m, const Undo *u);
int king_square(const Position *p, int color);
int mover_left_king_capturable(const Position *p);  /* after a move: can the side to move take the king? */

bb_t visible_squares(const Position *p, int color);  /* squares `color` observes */
void observation(const Position *p, int color, char out[65]);  /* arena view string */

void move_to_uci(Move m, char out[6]);

#endif
