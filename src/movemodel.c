/* mist-c: opponent move model features. See movemodel.h. */
#include "movemodel.h"

#include <string.h>

static const float value[6] = {1, 3, 3, 5, 9, 0};

/* Fitted by tools/fit_move_model.py on 2,029 arena games from 2026-09-30 to
 * 2026-10-10 (138,140 moves by every player but the random ones and
 * baseline.py). On 371 held-out games (25,678 moves) the full model gains
 * 0.631 nats per move over uniform (top-1 27.7%), the cheap one 0.493 (25.6%). */
static const float theta_full[MM_FEATURES] = {1.0539f, 3.6518f, 0.0000f, -0.6054f, -0.7420f, -0.2285f, -0.4249f, -0.3893f, 0.1147f, 2.3418f, 1.6564f, -0.5505f, -0.9329f, -0.1093f, 0.9448f, 1.5853f, 1.4877f, 1.1320f, 0.6865f, 1.0260f, -0.1586f, -2.1488f, -2.5339f};
static const float theta_cheap[MM_FEATURES] = {0.7705f, 3.6690f, 0.0000f, -0.5296f, -0.6920f, -0.2792f, -0.7406f, -0.4606f, 0.1304f, 2.3256f, 1.3647f, -0.6156f, -1.4760f, -0.1127f, 0.7298f, 1.5767f, 1.6693f, 1.0416f, 0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f};

void move_logits(const Position *p, const MoveList *l, Move previous, int full, float *out) {
    const float *theta = full ? theta_full : theta_cheap;
    float x[MM_FEATURES];
    for (int i = 0; i < l->count; ++i) {
        move_features(p, l->moves[i], previous, full, x);
        float z = 0;
        for (int k = 0; k < MM_FEATURES; ++k) z += theta[k] * x[k];
        out[i] = z;
    }
}

void move_features(const Position *p, Move m, Move previous, int full, float out[MM_FEATURES]) {
    memset(out, 0, sizeof(float) * MM_FEATURES);
    const int from = MOVE_FROM(m), to = MOVE_TO(m), promo = MOVE_PROMO(m);
    const int mover = p->side, type = p->piece[from];
    const int rank_from = mover == WHITE ? from / 8 : 7 - from / 8, rank_to = mover == WHITE ? to / 8 : 7 - to / 8;
    int captured = p->piece[to] != NO_PIECE && p->color[to] != mover ? p->piece[to] : NO_PIECE;
    if (type == PAWN && to == p->ep && p->piece[to] == NO_PIECE) captured = PAWN;
    if (captured != NO_PIECE) {
        out[MM_CAPTURE] = 1;
        if (captured == KING) out[MM_KING_CAPTURE] = 1;
        else out[MM_CAPTURED_VALUE] = value[captured] / 9.0f;
    }
    const int castle = type == KING && (to - from == 2 || from - to == 2);
    switch (type) {
        case KNIGHT: out[MM_KNIGHT] = 1; break;
        case BISHOP: out[MM_BISHOP] = 1; break;
        case ROOK: out[MM_ROOK] = 1; break;
        case QUEEN: out[MM_QUEEN] = 1; break;
        case KING: if (!castle) out[MM_KING] = 1; break;
        default: break;
    }
    if (castle) out[MM_CASTLE] = 1;
    if (type == PAWN && (to - from == 16 || from - to == 16)) out[MM_DOUBLE_PUSH] = 1;
    if (promo == QUEEN) out[MM_PROMOTE_QUEEN] = 1;
    else if (promo != NO_PIECE) out[MM_UNDERPROMOTE] = 1;
    out[MM_FORWARD] = (float)(rank_to - rank_from) / 7.0f;
    if (rank_to < rank_from) out[MM_RETREAT] = 1;
    {
        const float df = (float)(to % 8) - 3.5f, dr = (float)(to / 8) - 3.5f;
        const float d = (df < 0 ? -df : df) > (dr < 0 ? -dr : dr) ? (df < 0 ? -df : df) : (dr < 0 ? -dr : dr);
        out[MM_CENTRE] = (3.5f - d) / 3.0f;
    }
    if ((type == KNIGHT || type == BISHOP) && rank_from == 0) out[MM_DEVELOP] = 1;
    if (previous && MOVE_TO(previous) == to && captured != NO_PIECE) out[MM_RECAPTURE] = 1;
    if (type == PAWN && rank_to >= 5) out[MM_ADVANCED_PAWN] = 1;
    if (!full) return;
    const int other = mover ^ 1;
    if (type != PAWN && type != KING && is_attacked(p, from, other)) out[MM_ESCAPE] = 1;
    Position q = *p; Undo u; make_move(&q, m, &u);
    const int their_king = king_square(&q, other);
    if (their_king >= 0 && is_attacked(&q, their_king, mover)) out[MM_CHECK] = 1;
    if (is_attacked(&q, to, other)) {
        out[MM_INTO_ATTACK] = 1;
        const int moved = promo != NO_PIECE ? promo : type;
        out[MM_HANG_VALUE] = (moved == KING ? 0 : value[moved]) / 9.0f;
    }
    const int own_king = king_square(&q, mover);
    if (own_king >= 0 && is_attacked(&q, own_king, other)) out[MM_SELF_CAPTURABLE] = 1;
}
