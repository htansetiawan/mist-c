/* mist-c: growing-tree search (milestone M3, GT-CFR; paper 3.3, C.4).
 *
 * Starts from the two-ply belief subgame of search.c and alternates PCFR+
 * iterations with expansions. An expansion walks down from the root: chance by
 * its probabilities; the exploring player (alternating each expansion) mixes
 * uniform-over-support of its current strategy with a PUCT choice (current
 * strategy as the prior, sqrt(N(I))/(1+N(I,a)) bonus); the other player plays
 * its current strategy. The leaf reached gains the next player's moves as new
 * evaluated leaves. Information sets are keyed by each player's own history
 * (its moves and its views), never by the other side's hidden moves. The root
 * move is the last iterate's top action. Approximations: worlds come from our
 * posterior only, leaves are static evaluations (no Stockfish), and the PUCT
 * value term is replaced by the current strategy.
 */
#ifndef MIST_GTCFR_H
#define MIST_GTCFR_H

#include "search.h"

typedef struct {
    int samples;                 /* |I| */
    double seconds;              /* whole-decision budget */
    int max_nodes;               /* tree size cap */
    int expansions_per_round, iterations_per_round;
    unsigned long long seed;
} GtcfrParams;

int gtcfr_decide(const Belief *b, const GtcfrParams *params, SearchResult *out);

#endif
