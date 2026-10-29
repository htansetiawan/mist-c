/* mist-c: opponent move model features. See movemodel.h. */
#include "movemodel.h"

#include <string.h>

#include "movemodel_theta.h"

static const float value[6] = {1, 3, 3, 5, 9, 0};

/* Fitted by tools/fit_move_model.py on 2,029 arena games from 2026-09-30 to
 * 2026-10-10 (138,140 moves by every player but the random ones and
 * baseline.py). On 371 held-out games (25,678 moves) the full model gains
 * 0.658 nats per move over uniform (top-1 27.4%; 0.631 before the undefended,
 * king-zone and x-ray features), the cheap one 0.493 (25.6%). */

void move_logits(const Position *p, const MoveList *l, Move previous, int full, float *out) {
    const float *theta = full ? theta_full : theta_cheap;
    const float (*pf)[64] = full ? pst_from_full : pst_from_cheap;
    const float (*pt)[64] = full ? pst_to_full : pst_to_cheap;
    float x[MM_FEATURES];
    const bb_t vis = full ? visible_squares(p, p->side) : 0;
    for (int i = 0; i < l->count; ++i) {
        const Move m = l->moves[i];
        move_features_obs(p, m, previous, full, vis, x);
        float z = 0;
        for (int k = 0; k < MM_FEATURES; ++k) z += theta[k] * x[k];
        const int type = p->piece[MOVE_FROM(m)];
        z += pf[type][move_mirror_square(p->side, MOVE_FROM(m))] + pt[type][move_mirror_square(p->side, MOVE_TO(m))];
        out[i] = z;
    }
}

void move_features(const Position *p, Move m, Move previous, int full, float out[MM_FEATURES]) {
    move_features_obs(p, m, previous, full, full ? visible_squares(p, p->side) : 0, out);
}

#define BIT(sq) (1ULL << (sq))

/* Is `sq` attacked in `q` by a piece of `by` standing on a square in `vis`? */
static int attacked_from_visible(const Position *q, int sq, int by, bb_t vis) {
    for (bb_t b = q->by_color[by] & vis; b; b &= b - 1)
        if (attacks_from(q, __builtin_ctzll(b)) & BIT(sq)) return 1;
    return 0;
}

static int chebyshev(int a, int b) {
    const int df = (a & 7) - (b & 7), dr = (a >> 3) - (b >> 3);
    const int x = df < 0 ? -df : df, y = dr < 0 ? -dr : dr;
    return x > y ? x : y;
}

/* Distance from `sq` to the nearest piece of `color` on a square in `vis`; -1 if none. */
static int nearest_visible(const Position *q, int sq, int color, bb_t vis) {
    int best = -1;
    for (bb_t b = q->by_color[color] & vis; b; b &= b - 1) {
        const int d = chebyshev(sq, __builtin_ctzll(b));
        if (best < 0 || d < best) best = d;
    }
    return best;
}

void move_features_obs(const Position *p, Move m, Move previous, int full, bb_t vis, float out[MM_FEATURES]) {
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
    /* hanging: attacked and not defended */
    if (out[MM_INTO_ATTACK] > 0 && !is_attacked(&q, to, mover)) out[MM_UNDEFENDED] = out[MM_HANG_VALUE];
    /* the mover's own view: `vis` is what it sees before the move */
    if (out[MM_ESCAPE] > 0 && attacked_from_visible(p, from, other, vis)) out[MM_OBS_ESCAPE_SEEN] = 1;
    if (out[MM_INTO_ATTACK] > 0) {
        if (attacked_from_visible(&q, to, other, vis)) {
            out[MM_OBS_INTO_ATTACK_SEEN] = 1;
            out[MM_OBS_HANG_SEEN] = out[MM_HANG_VALUE];
            if (captured != NO_PIECE) out[MM_OBS_CAPTURE_DEFENDED_SEEN] = 1;
        } else if (captured != NO_PIECE) out[MM_OBS_CAPTURE_DEFENDED_UNSEEN] = 1;
    }
    if (out[MM_SELF_CAPTURABLE] > 0 && attacked_from_visible(&q, own_king, other, vis)) out[MM_OBS_SELF_CAPTURABLE_SEEN] = 1;
    {
        const bb_t targets = attacks_from(&q, to) & q.by_color[other] & vis;
        for (bb_t b = targets; b; b &= b - 1) {
            const int sq = __builtin_ctzll(b);
            if (q.piece[sq] != PAWN && q.piece[sq] != KING) {
                out[MM_OBS_THREAT_SEEN] = 1;
                if (value[q.piece[sq]] / 9.0f > out[MM_OBS_THREAT_SEEN_VALUE]) out[MM_OBS_THREAT_SEEN_VALUE] = value[q.piece[sq]] / 9.0f;
            }
        }
        {
            /* squares the mover does not see from which a queen or a knight could reach `to` */
            Position r = q;
            pos_remove(&r, to); pos_put(&r, to, mover, QUEEN);
            bb_t reach = attacks_from(&r, to);
            pos_remove(&r, to); pos_put(&r, to, mover, KNIGHT);
            reach |= attacks_from(&r, to);
            out[MM_OBS_FOG_EXPOSURE] = (float)__builtin_popcountll(reach & ~vis) / 27.0f;
        }
        const int before = nearest_visible(p, from, other, vis), after = nearest_visible(p, to, other, vis);
        if (before >= 0 && after >= 0) out[MM_OBS_APPROACH_SEEN] = (float)(before - after) / 7.0f;
        out[MM_OBS_REVEAL] = (float)__builtin_popcountll(visible_squares(&q, mover) & ~vis) / 16.0f;
    }
    if (their_king < 0) return;
    /* pressure next to their king, and a line through one of their pieces to
     * it (a pin or skewer: the threats a hidden piece prepares) */
    const bb_t hits = attacks_from(&q, to);
    {
        /* what the victim sees after the move: a check or a threat on a
         * 3+ piece from a square outside its view is a hidden threat */
        char seen[65];
        observation(&q, other, seen);
        if (seen[to] == '?') {
            if (out[MM_CHECK] > 0) out[MM_CHECK_UNSEEN] = 1;
            for (bb_t b = hits & q.by_color[other]; b; b &= b - 1) {
                const int sq = __builtin_ctzll(b);
                if (q.piece[sq] != PAWN && q.piece[sq] != KING) { out[MM_THREAT_UNSEEN] = 1; break; }
            }
        }
    }
    out[MM_KING_ZONE] = (float)__builtin_popcountll(hits & attacks_from(&q, their_king)) / 8.0f;
    const int moved = promo != NO_PIECE ? promo : type;
    if (moved == BISHOP || moved == ROOK || moved == QUEEN) {
        for (bb_t b = hits & q.by_color[other] & ~(1ULL << their_king); b; b &= b - 1) {
            Position r = q;
            pos_remove(&r, __builtin_ctzll(b));
            if (attacks_from(&r, to) & (1ULL << their_king)) { out[MM_XRAY_KING] = 1; break; }
        }
    }
}
