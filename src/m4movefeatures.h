#ifndef MIST_M4MOVEFEATURES_H
#define MIST_M4MOVEFEATURES_H
#include "movemodel.h"
enum {
    M4MM_HIDDEN_CHECK=MM_FEATURES, M4MM_SILENT_CHECK, M4MM_SILENT_MOVE,
    M4MM_CONCEALED_MOVER, M4MM_NEW_VISION, M4MM_CONCEALED_KING_ZONE,
    M4MM_FEATURES
};
/* Offline experiment: original full features plus six visibility interactions.
 * These condition on a hypothetical full state, just like the original prior;
 * they are not claimed to define an information-set-consistent actor policy.
 * Does not alter p, l or the active opponent model. */
void m4_move_features(const Position *p,const MoveList *l,Move previous,
                      float out[256][M4MM_FEATURES]);
#endif
