/* mist-c: an opponent move model for weighting the belief (the paper's
 * future work: "sample from P with a learned opponent model instead of
 * uniformly"). A conditional logit over the mover's legal moves,
 * P(m | s) = exp(theta . phi(s, m)) / sum over m' of exp(theta . phi(s, m')),
 * with hand-made features of the move (tools/move_features.c exports them,
 * tools/fit_move_model.py fits theta on arena games). Cheap features only, so
 * the belief update can normalise over every legal move. */
#ifndef MIST_MOVEMODEL_H
#define MIST_MOVEMODEL_H

#include "fow.h"

enum {
    MM_CAPTURE, MM_CAPTURED_VALUE, MM_KING_CAPTURE,
    MM_KNIGHT, MM_BISHOP, MM_ROOK, MM_QUEEN, MM_KING,
    MM_DOUBLE_PUSH, MM_CASTLE, MM_PROMOTE_QUEEN, MM_UNDERPROMOTE,
    MM_FORWARD, MM_RETREAT, MM_CENTRE, MM_DEVELOP, MM_RECAPTURE, MM_ADVANCED_PAWN,
    MM_CHECK, MM_ESCAPE, MM_INTO_ATTACK, MM_HANG_VALUE, MM_SELF_CAPTURABLE,
    MM_UNDEFENDED, MM_KING_ZONE, MM_XRAY_KING,
    /* the moved piece checks, or attacks a piece worth 3+, from a square the
     * victim's view does not show: the hidden threats the top teams play
     * (2.4 per 100 moves on the enemy king against our 0.8) and that a model
     * fitted without them rated at 3-6% */
    MM_CHECK_UNSEEN, MM_THREAT_UNSEEN,
    MM_FEATURES
};

/* Features of `m` for the side to move in `p`; `previous` is the other side's
 * last move (0 if unknown), for recaptures. `full` adds the attack features
 * (MM_CHECK..MM_XRAY_KING), which cost attack tests. */
void move_features(const Position *p, Move m, Move previous, int full, float out[MM_FEATURES]);
/* The piece-square tables (movemodel_theta.h) are written for White; a Black
 * mover's squares are mirrored by rank. */
static inline int move_mirror_square(int mover, int sq) { return mover == WHITE ? sq : sq ^ 56; }
/* theta . phi plus the piece-square terms, for every move of `l`. */
void move_logits(const Position *p, const MoveList *l, Move previous, int full, float *out);

#endif
