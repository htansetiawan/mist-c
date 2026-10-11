/* M4: order-2 knowledge closure and blueprint ownership. Paper C.6/Fig. 9.
 * At the frontier, BOTH players' history infosets must be supplied, including
 * the inactive player. GameNode.infoset only describes the mover and cannot
 * supply this information. Never substitute a board hash for a history key.
 */
#ifndef MIST_KLUSS_H
#define MIST_KLUSS_H
#include <stdint.h>
#include "cfr.h"
typedef struct {
    int node;
    uint64_t ours, theirs;
} KlussFrontier;

/* Keep I^2: distance 0 for our current infoset, distance 1 for additional
 * histories indistinguishable to the opponent, -1 otherwise. No transitive
 * closure through distance-1 worlds. O(R log R) time, O(R) temporary memory.
 * Returns kept count; -1 on invalid/absent current infoset or allocation error.
 * gadget_build then retains all descendants with player-0 nodes UNFROZEN. */
int kluss_select(const KlussFrontier *frontier,int count,uint64_t current,
                 unsigned char *keep,int *distance);

typedef struct {
    Game tree;                  /* immutable structural copy, raw P0 utilities */
    double *strategy;           /* behavior snapshot, independent of solver state */
    uint64_t *ours, *theirs;    /* history identities by node, supplied by caller */
    double expected_value;
    int valid;
} KlussBlueprint;
void kluss_blueprint_init(KlussBlueprint *b);
void kluss_blueprint_free(KlussBlueprint *b);
/* Atomically replace a snapshot after a completed search. Preserve the old
 * snapshot on invalid inputs. No regret, prediction, or average accumulators
 * are carried over. Raw leaf evals must be restored before capturing a gadget.
 * Board/leaf payload ownership remains with the frontend integration. */
int kluss_blueprint_capture(KlussBlueprint *b,const Game *g,const double *x,
                            const uint64_t *ours,const uint64_t *theirs);
#endif
