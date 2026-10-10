/* mist-c: leaf evaluation. See eval.h. Tables are this project's own: simple
 * advancement and centralisation shapes, not copied from any engine. */
#include "eval.h"

#include <math.h>

static const int kValue[6] = {100, 320, 330, 500, 900, 0};

/* Centralisation: 0 at the edge, 3 in the centre. */
static int centre(int sq) {
    const int f = sq % 8, r = sq / 8;
    const int df = f < 4 ? f : 7 - f, dr = r < 4 ? r : 7 - r;
    return df < dr ? df : dr;
}

static int placement(int type, int sq, int color, int endgame) {
    const int rank = color == WHITE ? sq / 8 : 7 - sq / 8;   /* 0 = own back rank */
    const int file = sq % 8;
    switch (type) {
        case PAWN: return 6 * rank + (file >= 2 && file <= 5 ? 4 * (rank >= 3) : 0) + (endgame ? 8 * rank : 0);
        case KNIGHT: return 10 * centre(sq) - 10;
        case BISHOP: return 6 * centre(sq) - 4;
        case ROOK: return rank == 6 ? 20 : 2 * centre(sq);
        case QUEEN: return 3 * centre(sq);
        case KING:
            if (endgame) return 10 * centre(sq);
            /* middlegame: home rank, near a corner (castled), behind pawns */
            return (rank == 0 ? 20 : -20 * rank) + (file <= 2 || file >= 6 ? 15 : 0);
        default: return 0;
    }
}

/* Pawns of `color` on the three files around its king, one or two ranks ahead. */
static int shelter(const Position *p, int color) {
    const int k = king_square(p, color);
    if (k < 0) return 0;
    const int kf = k % 8, kr = k / 8, step = color == WHITE ? 8 : -8;
    int s = 0;
    for (int df = -1; df <= 1; ++df) {
        const int f = kf + df;
        if (f < 0 || f > 7) continue;
        for (int d = 1; d <= 2; ++d) {
            const int sq = (kr * 8 + f) + d * step;
            if (sq < 0 || sq > 63) continue;
            if (p->piece[sq] == PAWN && p->color[sq] == color) { s += d == 1 ? 12 : 6; break; }
        }
    }
    return s;
}

/* Cheapest piece of `color` attacking `sq`, as a value; 0 if none. */
static int cheapest_attacker(const Position *p, int sq, int color) {
    int best = 0;
    for (bb_t b = p->by_color[color]; b; b &= b - 1) {
        const int from = __builtin_ctzll(b);
        if (!(attacks_from(p, from) & ((bb_t)1 << sq))) continue;
        const int v = p->piece[from] == KING ? 2000 : kValue[(int)p->piece[from]];
        if (!best || v < best) best = v;
    }
    return best;
}

int evaluate_cp(const Position *p, int us) {
    const int them = us ^ 1;
    int non_pawn = 0;
    for (int t = KNIGHT; t <= QUEEN; ++t) non_pawn += kValue[t] * __builtin_popcountll(p->by_type[t]);
    const int endgame = non_pawn <= 2600;
    int score = 0;
    for (int color = 0; color < 2; ++color) {
        const int sign = color == us ? 1 : -1;
        for (bb_t b = p->by_color[color]; b; b &= b - 1) {
            const int sq = __builtin_ctzll(b), t = p->piece[sq];
            score += sign * (kValue[t] + placement(t, sq, color, endgame));
        }
        if (!endgame) score += sign * shelter(p, color);
    }
    /* We move next: the best capture we have now (victim minus the attacker if
     * the square is defended), counted at 80%: the opponent cannot answer it. */
    int best_gain = 0;
    for (bb_t b = p->by_color[them]; b; b &= b - 1) {
        const int sq = __builtin_ctzll(b), t = p->piece[sq];
        if (t == KING) continue;
        const int attacker = cheapest_attacker(p, sq, us);
        if (!attacker) continue;
        const int gain = kValue[t] - (cheapest_attacker(p, sq, them) ? attacker : 0);
        if (gain > best_gain) best_gain = gain;
    }
    return score + best_gain * 4 / 5;
}

double evaluate(const Position *p, int us) { return tanh(evaluate_cp(p, us) / 400.0); }
