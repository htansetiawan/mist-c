#ifndef MIST_M4SAMPLE_H
#define MIST_M4SAMPLE_H
#include "belief.h"
typedef struct {
    size_t scanned;
    int complete, represented_moves, witnesses;
} M4WitnessStats;
/* Deterministic representatives of fatal root actions. known[a] says current
 * roots already contain a fatal witness for action a. Only observations-derived
 * belief is consulted. Returns distinct indices (highest-weight witness per
 * uncovered action, prioritized by number of actions represented). The optional
 * monotonic deadline bounds the scan; complete=0 explicitly reports truncation.
 * Does not claim posterior probabilities or exhaustive support on truncation.
 */
int m4_witness_sample(const Belief *b,const MoveList *moves,const unsigned char *known,
                      int limit,double deadline,int *indices,M4WitnessStats *stats);
#endif
