/* mist-c: exact belief (milestone M2).
 *
 * P = every position consistent with our observation history (paper, Section 3:
 * "the full set P of possible positions given the observations"). Updated
 * exactly: after the opponent moves, every position in P is expanded by every
 * opponent move and kept iff our view of the result equals the view received;
 * after our own move, the move is applied to every position and filtered the
 * same way. Positions are deduplicated by a Zobrist hash. If P would exceed
 * `cap`, the excess is dropped and `overflowed` is set (the regime in which an
 * enumerating engine can lose the true board).
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
    size_t count, capacity, cap;
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

#endif
