/* mist-c: exact belief (milestone M2), weighted.
 *
 * P = every position consistent with our observation history (paper, Section 3:
 * "the full set P of possible positions given the observations"). Updated
 * exactly: after the opponent moves, every position in P is expanded by every
 * opponent move and kept iff our view of the result equals the view received;
 * after our own move, the move is applied to every position and filtered the
 * same way. A move that leaves the mover's king capturable ends the game (the
 * referee's rule), so the game going on rules such positions out.
 *
 * Each position carries a log-weight: the opponent move model (movemodel.h)
 * scores every hidden opponent move, so a position reached by natural moves
 * outweighs one reached by odd ones; positions reached by several histories
 * add their weights. Past `cap`, positions are kept by weighted sampling
 * without replacement (the true board is the likeliest to survive) and
 * `overflowed` is set. Search samples its worlds by weight (belief_sample).
 */
#ifndef FOW_BELIEF_H
#define FOW_BELIEF_H

#include <stddef.h>
#include <stdint.h>

#include "fow.h"

typedef struct {
    Position *items;
    uint64_t *hist;     /* per position: hash of the OPPONENT's own moves and views so far
                           (its information state), so search can key its information
                           sets by history and not by the current view alone */
    float *logw;        /* per position: log-weight (NULL = uniform) */
    size_t count, capacity, cap;
    Move last_own;      /* our last move, for the model's recapture feature */
    uint64_t *set;      /* open-addressing hash set of (position, history) keys */
    size_t set_size, set_count;
    uint64_t rng;       /* past the cap, a uniform random sample is kept (reservoir) */
    int us;
    int overflowed;
} Belief;

void zobrist_init(void);
uint64_t position_key(const Position *p);
uint64_t view_hash(const char view[65]);
uint64_t history_mix(uint64_t h, uint64_t value);

void belief_init_start(Belief *b, int our_color, size_t cap);
void belief_free(Belief *b);
/* Returns the new size of P. */
size_t belief_after_opponent(Belief *b, const char view[65]);
size_t belief_after_own(Belief *b, Move m, const char view[65]);
int belief_contains(const Belief *b, const Position *p);
/* The weight share of `p` in P (0 if absent). */
double belief_share(const Belief *b, const Position *p);

/* K draws with replacement in proportion to the weights (all of P when it
 * has at most K positions): writes distinct indices and their probabilities
 * (multiplicity / K, or the normalised weights), returns how many. */
int belief_sample(const Belief *b, int K, uint64_t *rng, int *idx, double *prob);

/* Rebuilds P from the current view alone, for when the capped belief has
 * lost every position (past the cap the reservoir can drop the true board).
 * It is our turn. The opponent's unseen pieces - `their` counts each type it
 * still has - are placed on the squares we cannot see, pawns near their start
 * rank; a placement is kept iff our view of it equals `view`. Up to `target`
 * distinct positions from at most `tries` placements; histories restart from
 * the opponent's current view. Returns the new |P| (unchanged if none fit). */
size_t belief_resample(Belief *b, const char view[65], const int their[6], bb_t our_castle, size_t target, size_t tries);

/* The moves legal on every board the view allows; any other move can be
 * illegal on the true board, which loses the game. Generated on the visible
 * position (unseen squares empty) and kept iff we see every square the move
 * needs: its destination, the square a double push crosses, the squares
 * between king and rook. En passant only when the view proves it (the taken
 * pawn is seen though none of our pieces attacks it). Returns the count. */
int view_moves(const char view[65], int us, bb_t our_castle, MoveList *out);

#endif
