/* mist-c: what the front end tracks from the views it receives, apart from
 * the belief. The opponent's material (its start less our captures, which we
 * always see), the rooks we may still castle with, and how often each
 * arrangement of our pieces has followed our move (the referee's threefold
 * repetition needs ours to repeat). Kept apart from main.c so that every rule
 * is tested as White and as Black (tests/test_front.c).
 */
#ifndef MIST_FRONT_H
#define MIST_FRONT_H

#include "belief.h"

enum { FRONT_MAX_CONFIGS = 1024 };

typedef struct {
    int us;
    int their[6];                        /* pieces the opponent still has, by type */
    bb_t castle;                         /* our rooks that may still castle */
    uint64_t config[FRONT_MAX_CONFIGS];  /* arrangements of our pieces after our moves */
    int config_seen[FRONT_MAX_CONFIGS];
    int configs;
} Front;

void front_init(Front *f, int us);
/* What we see, as a position: our pieces and the visible enemy pieces. */
void front_view_position(const char view[65], int us, bb_t castle, Position *p);
/* A view arrived: castling ends where our king or a rook has left its square. */
void front_see(Front *f, const char view[65]);
/* We play `m` from `view`: count what it takes (en passant too), update
 * castling, and count the arrangement of our pieces it leaves. */
void front_play(Front *f, const char view[65], Move m);
/* Our material less theirs, in pawns (N = B = 3, R = 5, Q = 9). */
int front_lead(const Front *f, const char view[65]);
/* The moves among `moves` after which our pieces would stand as they already
 * have twice; returns how many were written to `out`. */
int front_repeats(const Front *f, const char view[65], const MoveList *moves, Move *out);
/* The move in `allowed` with the best evaluation on the visible position. */
Move front_best_allowed(const Front *f, const char view[65], const MoveList *allowed);
/* the same, skipping the `navoid` moves in `avoid` (the search's exclusions);
 * they are ignored only when they would leave nothing */
Move front_best_allowed_avoiding(const Front *f, const char view[65], const MoveList *allowed, const Move *avoid, int navoid);
/* the root exclusions: the repetition rule's moves and the guards' moves
 * united, into `out` (room for both lists); when the union would leave no
 * move the repetition rule yields and only the guards' moves are listed
 * (`yielded` set when that happened). Returns the count. */
int front_exclusions(const MoveList *allowed, const Move *repeats, int nrep, const Move *guards, int nguard, Move *out, int *yielded);

#endif
