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
 *
 * king_risk: our belief's chance that a move leaves our king capturable was
 * calibrated against v25, TA-P and mist-c itself (22 deaths expected by it on
 * the moves played, 18 happened) but not against Misty: 5.9 expected, 16
 * happened. A strong opponent's hidden moves set up these traps more often
 * than our move model (fitted on the whole arena) predicts, so the root's
 * losses can count kappa times.
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
    const Move *avoid;           /* root moves left out (e.g. a third repetition when ahead) */
    int avoid_count;             /* ignored if it would leave no move */
    double king_risk;            /* kappa: a root move that leaves our king capturable scores
                                    -kappa instead of -1 (values below 1 mean 1) */
} GtcfrParams;

int gtcfr_decide(const Belief *b, const GtcfrParams *params, SearchResult *out);

#endif

/* diagnostics (MIST_TRUTH_SEQ): when set, the search logs whether this 64-char board was among its sampled worlds */
extern const char *gtcfr_truth_board;
