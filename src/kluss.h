/* M4: order-2 knowledge closure and blueprint ownership. Paper C.6/Fig. 9.
 * At the frontier, BOTH players' history infosets must be supplied, including
 * the inactive player. GameNode.infoset only describes the mover and cannot
 * supply this information. Never substitute a board hash for a history key.
 */
#ifndef MIST_KLUSS_H
#define MIST_KLUSS_H
#include <stdint.h>
#include "cfr.h"
#include "gadget.h"
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

/* Prepare the carried portion of a new subgame. The adapter supplies an
 * antichain of histories at the new decision time; no board matching occurs
 * here. Fresh sampled worlds must still be added by the engine adapter.
 * Grouping is by the opponent's full history key, never by board/view alone.
 * Gifts are computed on the unpruned blueprint before building the gadget. */
typedef struct {
    Gadget gadget;
    GadgetRoot *roots;
    GadgetGroup *groups;
    uint64_t *opponent_keys;
    int *frontier_index,*distance;
    int root_count,group_count,current_count;
} KlussPrepared;
void kluss_prepared_init(KlussPrepared *p);
void kluss_prepared_free(KlussPrepared *p);
/* Initialize out before first use. Atomically replaces it on success;
 * preserves it on invalid metadata, undefined conditional support, malformed
 * histories or allocation failure. Seeds fresh minimizers with blueprint
 * behavior. O(nodes+edges+slots+frontier*log(frontier)) work, linear memory. */
int kluss_prepare(const KlussBlueprint *blueprint,const KlussFrontier *frontier,
                  int count,uint64_t current,KlussPrepared *out);
#endif
