/* mist-c: a first decision procedure (milestone M3, before GT-CFR expansion).
 *
 * Sample I from the exact belief P (uniformly, paper Section 3), build the
 * two-ply imperfect-information game -- our move (one information set: our
 * view is the same in every position of P), then the opponent's reply, its
 * information sets keyed by what the opponent sees after our move -- score
 * the leaves, solve it with PCFR+ and play from the last iterate. Leaving our
 * king capturable loses, leaving theirs wins (the referee's rule). GT-CFR
 * expansion (deeper, selective) and the KLUSS subgame come next.
 */
#ifndef MIST_SEARCH_H
#define MIST_SEARCH_H

#include "belief.h"

typedef struct {
    int samples;              /* |I| */
    int iterations;           /* PCFR+ iteration cap */
    unsigned long long seed;
    double seconds;           /* PCFR+ time budget (0 = run all iterations) */
    int material_only;        /* 1 = the material leaf (tests, comparison) */
} SearchParams;

typedef struct {
    Move move;
    double value;             /* root value for us under the last iterate, in [-1, 1] */
    double probability;       /* the move's weight in the last iterate */
    int samples, our_moves, opponent_infosets, nodes, iterations;
} SearchResult;

/* Leaf value for `us` in [-1, 1]: material, squashed. */
double evaluate_material(const Position *p, int us);
/* Returns 0 when there is no legal move. */
int search_decide(const Belief *b, const SearchParams *params, SearchResult *out);

#endif
