/* mist-c: opponent move model features. See movemodel.h. */
#include "movemodel.h"

#include <string.h>

static const float value[6] = {1, 3, 3, 5, 9, 0};

/* Refitted by tools/fit_move_model_stream.py (Newton, streamed) on 148,357 arena
 * games from 2026-09-01 to 2026-10-31 (9,901,850 moves by every player but us and
 * the random/baseline bots). On 26,029 held-out games (1,734,620 moves) the full
 * model gains 0.645 nats per move over uniform (top-1 27.1%; the 10 October fit on
 * 2,029 games scored 0.638 / 26.7% on the same moves), the cheap one 0.474
 * (24.8%; was 0.472 / 24.5%). */
static const float theta_full[MM_FEATURES] = {0.8256f, 6.1569f, 0.0000f, -0.5818f, -0.6813f, -0.1419f, -0.3730f, -0.3004f, 0.1104f, 2.5549f, 2.8820f, -1.1917f, -0.8334f, -0.1136f, 0.9504f, 1.5825f, 1.4175f, 1.2139f, 0.7535f, 1.0183f, -0.0249f, -1.5504f, -3.2408f, -3.1171f, 0.3764f, 0.2498f, 0.0000f, 0.0000f};
static const float theta_cheap[MM_FEATURES] = {0.5520f, 4.5402f, 0.0000f, -0.5542f, -0.6984f, -0.2373f, -0.7377f, -0.3945f, 0.1346f, 2.4762f, 1.8108f, -1.7176f, -1.5143f, -0.1353f, 0.7632f, 1.6151f, 1.7061f, 1.1334f, 0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f};

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
    /* hanging: attacked and not defended */
    if (out[MM_INTO_ATTACK] > 0 && !is_attacked(&q, to, mover)) out[MM_UNDEFENDED] = out[MM_HANG_VALUE];
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
