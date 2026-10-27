#define _POSIX_C_SOURCE 199309L  /* clock_gettime under -std=c11 */
/* mist-c: referee-protocol front end (milestone M5).
 *
 * The course referee sends "fow", then "<w|b> <base> <increment>"; the bot
 * answers "ready". Then a 64-character view arrives before every ply, and our
 * remaining seconds when it is our turn; we answer "move <uci>". The exact
 * belief is updated from each view: the first view is the start (nothing
 * moved), a view following our own move is updated with that move, any other
 * view by every opponent move consistent with it. If the capped belief ever
 * loses every position, it is rebuilt from the current view (belief_resample),
 * with the opponent's material counted from our captures. Whatever decides,
 * the move sent must be one view_moves allows (or, with the belief exact,
 * one legal on every board in it): v1 lost two games by pushing a pawn into an
 * unseen piece after its belief ran empty. When ahead, moves that would make
 * our pieces stand as they already have twice are left out: v1 drew three
 * games by repetition, two of them seven points up. The opponent update is
 * paid from our clock: P is first thinned to what half the move's budget can
 * expand (v3 lost three games on time, its 200,000-position updates taking
 * 3-4 s against a 2 s increment).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "front.h"
#include "gtcfr.h"
#include "search.h"
#include "sfeval.h"

static int is_view(const char *s) {
    if (strlen(s) != 64) return 0;
    for (int i = 0; i < 64; ++i) if (!strchr("PNBRQKpnbrqk-?", s[i])) return 0;
    return 1;
}

/* Legal in every position of P: with P exact (never capped) the true board is
 * among them, so this proves legality where the view alone cannot (an en
 * passant whose taken pawn another piece of ours also attacks). */
static int legal_everywhere(const Belief *b, Move m) {
    for (size_t i = 0; i < b->count; ++i) {
        MoveList l; generate_moves(&b->items[i], &l);
        int found = 0;
        for (int j = 0; j < l.count && !found; ++j) found = l.moves[j] == m;
        if (!found) return 0;
    }
    return b->count > 0;
}

/* The weight share of P on which `m` leaves our king capturable (the view
 * proves `m` legal on every board of P, so it can be played on each). */
static double king_risk(const Belief *b, Move m) {
    double total = 0, lost = 0;
    for (size_t i = 0; i < b->count; ++i) {
        const double w = b->logw ? exp((double)b->logw[i]) : 1.0;
        Position p = b->items[i];
        Undo u;
        make_move(&p, m, &u);
        total += w;
        if (mover_left_king_capturable(&p)) lost += w;
    }
    return total > 0 ? lost / total : 0;
}

/* Expected material `m` loses on the next ply, in pawns, over at most `cap`
 * boards of P evenly strided (weights kept): the opponent's best immediate
 * capture (the piece taken less the capturer where we can take it back)
 * less what `m` itself takes on that board. King captures are the king
 * guard's business and count 0 here. Of 283 decisive bad moves in
 * our v25-v33 losses, 273 lost material to a piece the search's 32 sampled
 * worlds did not hold often enough; all of P sees it more often. */
static const int piece_pawns[6] = {1, 3, 3, 5, 9, 0};
static double material_risk(const Belief *b, Move m, size_t cap) {
    const size_t stride = b->count > cap ? (b->count + cap - 1) / cap : 1;
    double total = 0, lost = 0;
    for (size_t i = 0; i < b->count; i += stride) {
        const double w = b->logw ? exp((double)b->logw[i]) : 1.0;
        Position p = b->items[i];
        int got = p.piece[MOVE_TO(m)] != NO_PIECE && p.color[MOVE_TO(m)] != p.side ? piece_pawns[p.piece[MOVE_TO(m)]] : 0;
        if (!got && p.piece[MOVE_FROM(m)] == PAWN && MOVE_TO(m) == p.ep) got = 1;
        Undo u;
        make_move(&p, m, &u);
        total += w;
        if (mover_left_king_capturable(&p)) continue;
        const int us = p.side ^ 1;
        MoveList l; generate_moves(&p, &l);
        int best = 0;
        for (int j = 0; j < l.count; ++j) {
            const int to = MOVE_TO(l.moves[j]), from = MOVE_FROM(l.moves[j]);
            int taken = p.piece[to] != NO_PIECE && p.color[to] == us ? p.piece[to] : NO_PIECE;
            if (taken == NO_PIECE && p.piece[from] == PAWN && to == p.ep) taken = PAWN;
            if (taken == NO_PIECE || taken == KING) continue;
            int gain = piece_pawns[taken];
            if (gain <= best) continue;
            Position q = p; Undo v; make_move(&q, l.moves[j], &v);
            if (mover_left_king_capturable(&q)) continue;   /* a suicidal capture: won, not lost (as best_capture_net) */
            if (is_attacked(&q, to, us)) gain -= piece_pawns[p.piece[from]];
            if (gain > best) best = gain;
        }
        lost += w * (best - got);   /* the net of the exchange this board sees */
    }
    return total > 0 ? lost / total : 0;
}

/* Queen guard. The share of P (strided to `cap`; the fitted weights and the
 * sharper weight^temp, as king_risks) on which `m` loses our queen on the
 * next ply for `threshold` pawns or more net: the opponent's best capture of a
 * square holding our queen nets 9 less the capturer where we can take it back,
 * less what `m` itself took. A world where `m` takes the king is won; one
 * where `m` leaves our king capturable is the king guard's. In v33 and v34's
 * 608 arena games against the top six our queen was lost outright in 42%
 * (we scored 0.01 in those games) and theirs in 10%; in 99 of 100 the taker
 * stood on a square we could not see when we moved, usually a defender of a
 * pawn or piece the queen took, and the search's few sampled worlds held it
 * too rarely. Measured on all of P the defender is there. */
static void queen_risks(const Belief *b, Move m, size_t cap, double temp, int threshold, double *r1, double *rt, double *ru) {
    const size_t stride = b->count > cap ? (b->count + cap - 1) / cap : 1;
    double t1 = 0, l1 = 0, tt = 0, lt = 0, tu = 0, lu = 0;
    for (size_t i = 0; i < b->count; i += stride) {
        const double lw = b->logw ? (double)b->logw[i] : 0.0;
        const double wt = exp(lw), w1 = exp(lw / temp);
        Position p = b->items[i];
        t1 += w1; tt += wt; tu += 1;
        const int to = MOVE_TO(m);
        if (p.piece[to] == KING && p.color[to] != p.side) continue;   /* takes the king: won */
        int got = p.piece[to] != NO_PIECE && p.color[to] != p.side ? piece_pawns[p.piece[to]] : 0;
        if (!got && p.piece[MOVE_FROM(m)] == PAWN && to == p.ep) got = 1;
        Undo u;
        make_move(&p, m, &u);
        if (mover_left_king_capturable(&p)) continue;
        const int us = p.side ^ 1;
        int best = 0;
        for (int sq = 0; sq < 64; ++sq) {
            if (p.piece[sq] != QUEEN || p.color[sq] != us || !is_attacked(&p, sq, p.side)) continue;
            MoveList l; generate_moves(&p, &l);
            for (int j = 0; j < l.count; ++j) {
                if (MOVE_TO(l.moves[j]) != sq) continue;
                int gain = piece_pawns[QUEEN];
                Position q = p; Undo v; make_move(&q, l.moves[j], &v);
                if (mover_left_king_capturable(&q)) continue;   /* a suicidal capture: won, not lost (as best_capture_net) */
                if (is_attacked(&q, sq, us)) gain -= piece_pawns[p.piece[MOVE_FROM(l.moves[j])]];
                if (gain > best) best = gain;
            }
        }
        if (best - got >= threshold) { l1 += w1; lt += wt; lu += 1; }
    }
    *r1 = t1 > 0 ? l1 / t1 : 0;
    *rt = tt > 0 ? lt / tt : 0;
    if (ru) *ru = tu > 0 ? lu / tu : 0;
}

/* Piece guard (v20). As queen_risks, for our rooks, bishops and knights: the
 * share of P (strided to `cap`, the same two weightings) on which, after `m`,
 * the opponent's best capture of one of them nets `threshold` pawns or more:
 * the piece less the capturer where we can take it back, less what `m` itself
 * took. The three terms partition the worlds: one where `m` takes the king is
 * won; one where `m` leaves our king capturable is the king guard's; one
 * where the queen can be taken for `queen_threshold` or more net is the queen
 * term's and is not counted here even if a rook or minor also hangs on it;
 * the piece term counts only the remaining worlds. So the combined risk is a
 * weighted share of disjoint world events, never above 1 on any world. An
 * enemy capture that leaves the enemy king capturable (the king taking a
 * defended piece, a pinned piece taking) is a won game, not a loss: all three
 * capture helpers (queen_risks, best_capture_net here, material_risk) skip
 * it, so the queen's world is the same event for the queen term and for this
 * term's exclusion. v35 charged such captures (Astra, 27 Oct, r3 bench). In
 * v35's games against the top six 54% became queen trades that we then lost
 * on material (scored 0.20-0.28): the guard covered the king and the queen,
 * and a rook or minor left to an unseen attacker was the search's few sampled
 * worlds' business. Measured on all of P the attacker is there. */
static int best_capture_net(const Position *p, int sq, int us, MoveList *l, int *generated) {
    /* the opponent's best net for taking our piece on `sq` (0 if it is not attacked) */
    if (!is_attacked(p, sq, p->side)) return 0;
    if (!*generated) { generate_moves(p, l); *generated = 1; }
    int best = 0;
    for (int j = 0; j < l->count; ++j) {
        if (MOVE_TO(l->moves[j]) != sq) continue;
        int gain = piece_pawns[p->piece[sq]];
        Position q = *p; Undo v; make_move(&q, l->moves[j], &v);
        /* a capture that leaves the capturer's own king to be taken (the king
         * taking a defended piece, a pinned piece taking) ends the game for us
         * on the next ply: no loss of the piece (Astra, 27 Oct, r3 bench) */
        if (mover_left_king_capturable(&q)) continue;
        if (is_attacked(&q, sq, us)) gain -= piece_pawns[p->piece[MOVE_FROM(l->moves[j])]];
        if (gain > best) best = gain;
    }
    return best;
}
static void piece_risks(const Belief *b, Move m, size_t cap, double temp, int threshold, int queen_threshold, double *r1, double *rt, double *ru) {
    const size_t stride = b->count > cap ? (b->count + cap - 1) / cap : 1;
    double t1 = 0, l1 = 0, tt = 0, lt = 0, tu = 0, lu = 0;
    for (size_t i = 0; i < b->count; i += stride) {
        const double lw = b->logw ? (double)b->logw[i] : 0.0;
        const double wt = exp(lw), w1 = exp(lw / temp);
        Position p = b->items[i];
        t1 += w1; tt += wt; tu += 1;
        const int to = MOVE_TO(m);
        if (p.piece[to] == KING && p.color[to] != p.side) continue;   /* takes the king: won */
        int got = p.piece[to] != NO_PIECE && p.color[to] != p.side ? piece_pawns[p.piece[to]] : 0;
        if (!got && p.piece[MOVE_FROM(m)] == PAWN && to == p.ep) got = 1;
        Undo u;
        make_move(&p, m, &u);
        if (mover_left_king_capturable(&p)) continue;
        const int us = p.side ^ 1;
        int best = 0, generated = 0, queens = 0;
        MoveList l;
        for (int sq = 0; sq < 64 && !queens; ++sq)   /* the queen term's world: not counted here */
            if (p.piece[sq] == QUEEN && p.color[sq] == us && best_capture_net(&p, sq, us, &l, &generated) - got >= queen_threshold) queens = 1;
        if (queens) continue;
        for (int sq = 0; sq < 64; ++sq) {
            const int pc = p.piece[sq];
            if ((pc != ROOK && pc != BISHOP && pc != KNIGHT) || p.color[sq] != us) continue;
            /* this piece cannot carry the net past the threshold, or past the best seen */
            if (piece_pawns[pc] - got < threshold || piece_pawns[pc] <= best) continue;
            const int net = best_capture_net(&p, sq, us, &l, &generated);
            if (net > best) best = net;
        }
        if (best - got >= threshold) { l1 += w1; lt += wt; lu += 1; }
    }
    *r1 = t1 > 0 ? l1 / t1 : 0;
    *rt = tt > 0 ? lt / tt : 0;
    if (ru) *ru = tu > 0 ? lu / tu : 0;
}

/* Diagnostics (MIST_TRUTH_SEQ: the true boards of the game, one 64-character
 * string per view in the protocol's order, comma separated): does P hold the
 * true position after each update, with what weight share and at what rank. */
static int position_is(const Position *p, const char *board) {
    for (int sq = 0; sq < 64; ++sq) {
        const char c = board[sq];
        if (c == '-') { if (p->piece[sq] != NO_PIECE) return 0; continue; }
        const char *t = strchr("PNBRQK", c >= 'a' ? c - 32 : c);
        if (!t || p->piece[sq] != (int)(t - "PNBRQK") || p->color[sq] != (c >= 'a' ? BLACK : WHITE)) return 0;
    }
    return 1;
}
static void truth_report(const Belief *b, const char *seq, int view_index) {
    const char *board = seq;
    for (int i = 0; i < view_index && board; ++i) { board = strchr(board, ','); if (board) ++board; }
    if (!board || strlen(board) < 64) return;
    double total = 0, mine = -1; size_t where = 0;
    for (size_t i = 0; i < b->count; ++i) {
        const double w = b->logw ? exp((double)b->logw[i]) : 1.0;
        total += w;
        if (mine < 0 && position_is(&b->items[i], board)) { mine = w; where = i; }
    }
    if (mine < 0) { fprintf(stderr, "mist: truth v=%d |P|=%zu%s LOST\n", view_index, b->count, b->overflowed ? "(capped)" : ""); return; }
    size_t rank = 1;
    for (size_t i = 0; i < b->count; ++i) if (i != where && (b->logw ? exp((double)b->logw[i]) : 1.0) > mine) ++rank;
    fprintf(stderr, "mist: truth v=%d |P|=%zu%s share=%.5f rank=%zu\n", view_index, b->count, b->overflowed ? "(capped)" : "", total > 0 ? mine / total : 0, rank);
}

/* king_risk on at most `cap` boards of P, evenly strided, under the belief's
 * weights (*rt: weight^temp of the fitted model, belief.c) and under the
 * fitted weighting recovered as weight^(1/temp) (*r1) */
/* Three weightings of the strided sample of P: the fitted weights (r1), the
 * sharper weight^temp the search samples by (rt), and uniform over the
 * consistent boards (ru, v15): a hidden attacker the move model rates as
 * implausible still sits on a share of the boards, and four of v13's seven
 * queen losses as Black had the truth in P at a weight of 0.07% or less. */
static void king_risks(const Belief *b, Move m, size_t cap, double temp, double *r1, double *rt, double *ru) {
    const size_t stride = b->count > cap ? (b->count + cap - 1) / cap : 1;
    double t1 = 0, l1 = 0, tt = 0, lt = 0, tu = 0, lu = 0;
    for (size_t i = 0; i < b->count; i += stride) {
        const double lw = b->logw ? (double)b->logw[i] : 0.0;
        const double wt = exp(lw), w1 = exp(lw / temp);
        Position p = b->items[i];
        Undo u;
        make_move(&p, m, &u);
        t1 += w1; tt += wt; tu += 1;
        if (mover_left_king_capturable(&p)) { l1 += w1; lt += wt; lu += 1; }
    }
    *r1 = t1 > 0 ? l1 / t1 : 0;
    *rt = tt > 0 ? lt / tt : 0;
    if (ru) *ru = tu > 0 ? lu / tu : 0;
}

static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

/* (remaining - reserve) / divisor + 80% of the increment, capped at max_seconds
 * and at 30% of what is left. The arena games are decided early: of 283
 * decisive bad moves in our v25-v33 losses, half came by our 10th decision and
 * 90% by the 18th, while remaining/30 left a median 77 s unused at the end
 * (v33: 85 s after 30 decisions, 42 s after 70). Divisor 20 spends the full
 * max_seconds on the first ten decisions and, by the model fitted on v33's
 * clocks, still keeps 20 s in a 100-decision game; on the regression suite
 * 1 s -> 6 s a decision fixed 12 and broke 5 of the material losses.
 * MIST_DIVISOR and MIST_RESERVE set the shape (30 and 0 = v9/v10). */
static double budget_divisor = 20.0, budget_reserve = 0.0;
static double move_budget(double remaining, double increment, double max_seconds) {
    double budget = (remaining - budget_reserve) / budget_divisor + 0.8 * increment;
    if (budget < 0.3) budget = 0.3;
    if (budget > max_seconds) budget = max_seconds;
    if (budget > remaining * 0.3) budget = remaining * 0.3;
    return budget;
}

typedef struct {
    double least, leastt, leastu;   /* least combined risk under the fitted, sharpened and uniform
                                       weightings (over v35's survivors when the piece term is on) */
    double kleast, kleastt;         /* least king risk, fitted and sharpened */
    double kqleast, kqleastt;       /* least king + queen risk */
    int dropped, kqdropped;         /* moves out under the whole rule; under v35's king + queen rule */
} GuardStats;

/* The root guard rule on per-move risks (count moves; fitted, sharpened and
 * uniform weightings of the king, queen and piece shares). out[i] = 1 leaves
 * move i out; kqout[i] = 1 is v35's verdict on king + lam x queen alone.
 * Two weightings: the fitted one catches a check the belief gives a few
 * percent (the a5 queen check after 1.d4: 3%); the sharper one (weight^temp)
 * keeps only the moves safe where the belief is concentrated. A move is out
 * when it is above the safest under either; if that leaves nothing, under the
 * sharper one alone. v35's rule runs first on king + queen; the piece term
 * (lamp > 0) then only narrows among its survivors, so the allowed set is a
 * subset of v35's: measured against all moves it brought back a 5.7% king
 * risk for a piece risk (05i9doxr:22, 26 Oct). With lamp 0 this is v35. */
static void guard_rule(int count,
                       const double *krisk, const double *kriskt, const double *krisku,
                       const double *qrisk, const double *qriskt, const double *qrisku,
                       const double *prisk, const double *priskt, const double *prisku,
                       double lam, double lamp, double guard, double uguard,
                       int *out, int *kqout, GuardStats *st) {
    double risk[256], riskt[256], risku[256], rkq[256], rkqt[256], rkqu[256];
    double least = 1e9, leastt = 1e9, leastu = 1e9, kleast = 1e9, kleastt = 1e9, kqleast = 1e9, kqleastt = 1e9, kquleast = 1e9;
    for (int i = 0; i < count; ++i) {
        rkq[i] = krisk[i] + lam * qrisk[i];
        rkqt[i] = kriskt[i] + lam * qriskt[i];
        rkqu[i] = krisku[i] + lam * qrisku[i];
        risk[i] = rkq[i] + lamp * prisk[i];
        riskt[i] = rkqt[i] + lamp * priskt[i];
        risku[i] = rkqu[i] + lamp * prisku[i];
        if (risk[i] < least) least = risk[i];
        if (riskt[i] < leastt) leastt = riskt[i];
        if (risku[i] < leastu) leastu = risku[i];
        if (krisk[i] < kleast) kleast = krisk[i];
        if (kriskt[i] < kleastt) kleastt = kriskt[i];
        if (rkq[i] < kqleast) kqleast = rkq[i];
        if (rkqt[i] < kqleastt) kqleastt = rkqt[i];
        if (rkqu[i] < kquleast) kquleast = rkqu[i];
    }
    /* v35's rule first, on king + queen: a move it leaves out stays out */
    int dropped = 0, kqdropped = 0;
    for (int pass = 0; pass < 2; ++pass) {
        kqdropped = 0;
        for (int i = 0; i < count; ++i) {
            kqout[i] = rkqt[i] > kqleastt + guard
                     || (pass == 0 && (rkq[i] > kqleast + guard || (uguard >= 0 && rkqu[i] > kquleast + uguard)));
            kqdropped += kqout[i];
        }
        if (kqdropped < count) break;
    }
    for (int i = 0; i < count; ++i) out[i] = kqout[i];
    dropped = kqdropped;
    if (lamp > 0) {
        least = leastt = leastu = 1e9;
        for (int i = 0; i < count; ++i) if (!kqout[i]) {
            if (risk[i] < least) least = risk[i];
            if (riskt[i] < leastt) leastt = riskt[i];
            if (risku[i] < leastu) leastu = risku[i];
        }
        for (int pass = 0; pass < 2; ++pass) {
            dropped = 0;
            for (int i = 0; i < count; ++i) {
                out[i] = kqout[i] || riskt[i] > leastt + guard
                      || (pass == 0 && (risk[i] > least + guard || (uguard >= 0 && risku[i] > leastu + uguard)));
                dropped += out[i];
            }
            if (dropped < count) break;
        }
    }
    st->least = least; st->leastt = leastt; st->leastu = leastu;
    st->kleast = kleast; st->kleastt = kleastt; st->kqleast = kqleast; st->kqleastt = kqleastt;
    st->dropped = dropped; st->kqdropped = kqdropped;
}

int main(void) {
    char line[256];
    if (!fgets(line, sizeof line, stdin) || strncmp(line, "fow", 3) != 0) return 0;
    char side[4]; double base = 180, increment = 2;
    if (!fgets(line, sizeof line, stdin) || sscanf(line, "%3s %lf %lf", side, &base, &increment) < 1) return 0;
    const int us = side[0] == 'w' ? WHITE : BLACK;
    fow_init(); zobrist_init();
    const size_t cap = getenv("MIST_CAP") ? (size_t)strtoull(getenv("MIST_CAP"), NULL, 10) : 200000;
    const double max_seconds = getenv("MIST_MAX_SECONDS") ? atof(getenv("MIST_MAX_SECONDS")) : 10.0;
    if (getenv("MIST_DIVISOR")) budget_divisor = atof(getenv("MIST_DIVISOR"));
    if (getenv("MIST_RESERVE")) budget_reserve = atof(getenv("MIST_RESERVE"));
    const int repeat_lead = getenv("MIST_REPEAT_LEAD") ? atoi(getenv("MIST_REPEAT_LEAD")) : 2;
    /* a root move that leaves our king capturable counts kappa losses (gtcfr.h) */
    const double kappa = getenv("MIST_KING_RISK") ? atof(getenv("MIST_KING_RISK")) : 2.0;
    /* MIST_KING_GUARD: leave out moves this far above the safest king risk (negative: off) */
    const double guard = getenv("MIST_KING_GUARD") ? atof(getenv("MIST_KING_GUARD")) : 0.02;
    /* MIST_GUARD_TEMP: the sharper weighting's temperature (default: the belief's) */
    const double guard_temp = getenv("MIST_GUARD_TEMP") ? atof(getenv("MIST_GUARD_TEMP")) : belief_temperature();
    /* MIST_MATERIAL_GUARD: leave out moves whose expected one-ply material
     * loss on P exceeds the least by this many pawns (negative: off) */
    const double mguard = getenv("MIST_MATERIAL_GUARD") ? atof(getenv("MIST_MATERIAL_GUARD")) : -1.0;
    const size_t mcap = getenv("MIST_MATERIAL_CAP") ? (size_t)strtoull(getenv("MIST_MATERIAL_CAP"), NULL, 10) : 5000;
    /* MIST_QUEEN_GUARD: leave out moves whose share of P losing the queen
     * (queen_risks) is this far above the least, under either weighting as the
     * king guard (negative: off). MIST_QUEEN_LOSS: the net pawns that count as
     * losing it. MIST_QUEEN_CAP: boards of P measured. Calibrated on 64
     * recorded queen losses (truth in P for 55): 0.05 vetoes the losing move in
     * 39 of the 55 and only 1% of the moves played where no queen was lost;
     * on 114 v34 losses it had never seen, the losing move was repeated 16
     * times instead of 93 and the loss on the true board fell from 440 to
     * 197 centipawns (worse in 2 decisions, better by 300+ in 36). */
    const double qguard = getenv("MIST_QUEEN_GUARD") ? atof(getenv("MIST_QUEEN_GUARD")) : 0.05;
    /* MIST_QUEEN_WEIGHT: v14 folds the queen risk into the king guard's rule
     * as one risk, king + weight x queen, instead of a second guard with its
     * own margin. v13's separate guard gave the king guard the last word, so
     * when every queen-saving move carried a few percent of king risk the
     * queen guard was dropped whole and a certain queen loss was played (v13
     * as Black: 2.5/12 vs the v34 bytes, 3/10 vs Misty; p3 decision 11).
     * A lost queen is worth about nine tenths of a lost game to us (we score
     * 0.01 in those games), so 0.9; a hidden check still outweighs it. */
    const double qweight = getenv("MIST_QUEEN_WEIGHT") ? atof(getenv("MIST_QUEEN_WEIGHT")) : 0.9;
    /* MIST_GUARD_UNIFORM (v15): a third clause of the guards' rule under the
     * uniform weighting of P's boards, with its own margin (negative: off).
     * Coarser than the weighted shares, so its margin is larger. */
    const double uguard = getenv("MIST_GUARD_UNIFORM") ? atof(getenv("MIST_GUARD_UNIFORM")) : -1.0;
    /* v16 tried 0.02 by default. Suite vs v14 (paired): check 60 -> 58 of
     * 300 (fixed 4, broke 2), fatal 48 -> 44 of 99 (fixed 4, broke 0),
     * material cp 132.6 -> 133.0; v35's two 8-ply arena losses as Black were
     * both a recapture that opened a line from our king to an unseen queen
     * after an early sortie (Qd3-e3; Qa4) the move model rates implausible
     * (truth weight 0.0006 and 0.0004 in P), and the uniform share refuses
     * the second (one board in 18) but not the first (one in 132). In play
     * it lost: 16.5/40 vs the v35 bytes (White 10/20, Black 6.5/20), so the
     * suite's gain is over-caution over a whole game. Withdrawn; off. */
    const int qloss = getenv("MIST_QUEEN_LOSS") ? atoi(getenv("MIST_QUEEN_LOSS")) : 4;
    const size_t qcap = getenv("MIST_QUEEN_CAP") ? (size_t)strtoull(getenv("MIST_QUEEN_CAP"), NULL, 10) : 20000;
    /* MIST_PIECE_GUARD (v20): a third term of the combined risk, lambda x the
     * share of P on which the move loses a rook, bishop or knight outright
     * (piece_risks; negative: off). MIST_PIECE_LOSS: the net pawns that count
     * as losing it (3: a clean minor or better; a plain exchange nets 2 and
     * does not count). MIST_PIECE_CAP: boards of P measured. lambda is fitted
     * from our score against the top six after a clean rook or minor loss,
     * relative to the queen's 0.9; off until the candidate ships. */
    const double pguard = getenv("MIST_PIECE_GUARD") ? atof(getenv("MIST_PIECE_GUARD")) : 0.758;   /* v20 B2 default: the fitted lambda; negative turns the term off */
    const int ploss = getenv("MIST_PIECE_LOSS") ? atoi(getenv("MIST_PIECE_LOSS")) : 3;
    const size_t pcap = getenv("MIST_PIECE_CAP") ? (size_t)strtoull(getenv("MIST_PIECE_CAP"), NULL, 10) : 20000;
    /* decision replay (tests): our first decisions play these moves (comma
     * separated UCI, as the game went) and the next one is searched */
    const char *forced = getenv("MIST_FORCE_MOVES");
    const char *truth_seq = getenv("MIST_TRUTH_SEQ");
    /* MIST_STOCKFISH=path: Stockfish scores the search's leaves (paper, 3.3) */
    if (getenv("MIST_STOCKFISH") && *getenv("MIST_STOCKFISH"))
        fprintf(stderr, "mist: stockfish %s\n", sf_start(getenv("MIST_STOCKFISH")) ? "ready" : "failed to start; our evaluator scores the leaves");
    Belief b; belief_init_start(&b, us, cap);
    static Front front;
    front_init(&front, us);
    printf("ready\n"); fflush(stdout);

    char view[65] = "";
    int views = 0, moved = 0, decisions = 0, rebuilds = 0;
    Move last = 0;
    unsigned long long seed = getenv("FOW_SEED") ? strtoull(getenv("FOW_SEED"), NULL, 10) : 1;
    double clock_left = base;       /* our clock when the next view comes, from the last reading */
    double per_position = 2e-5;     /* seconds the opponent update costs per position of P, measured */
    double view_at = now_seconds(); /* our clock runs from the view before our turn */
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (is_view(line)) {
            memcpy(view, line, 65);
            view_at = now_seconds();
            if (views++ > 0 && b.count > 0) {
                const double update_started = now_seconds();
                const size_t before = b.count;
                if (moved) belief_after_own(&b, last, view);
                else {
                    const size_t afford = (size_t)(0.5 * move_budget(clock_left, increment, max_seconds) / per_position);
                    if (b.count > afford && b.count > 2000) {
                        const size_t was = b.count;
                        belief_shrink(&b, afford > 2000 ? afford : 2000);
                        fprintf(stderr, "mist: clock %.1f s: P thinned from %zu to %zu before the update\n", clock_left, was, b.count);
                    }
                    const size_t parents = b.count;
                    const double started = now_seconds();
                    belief_after_opponent(&b, view);
                    if (parents >= 1000) per_position = 0.5 * per_position + 0.5 * (now_seconds() - started) / (double)parents;
                }
                if (getenv("MIST_TIMING"))   /* diagnostics: the cost of each update */
                    fprintf(stderr, "mist: timing %s update %.3f s |P| %zu -> %zu\n", moved ? "own" : "opp", now_seconds() - update_started, before, b.count);
            }
            front_see(&front, view);
            if (b.count == 0 && !moved && views > 1) {
                /* our turn and nothing left: rebuild from what we see */
                const size_t n = belief_resample(&b, view, front.their, front.castle, 2000, 60000);
                fprintf(stderr, "mist: belief empty, rebuilt %zu positions from the view (rebuild %d)\n", n, ++rebuilds);
            }
            if (truth_seq && views > 1) truth_report(&b, truth_seq, views - 1);
            moved = 0;
            continue;
        }
        char *end; const double reported = strtod(line, &end);
        if (end == line) continue;
        /* our turn; the clock ran while we updated P. A larger sample of P
         * when there is time for it */
        const double remaining = reported - (now_seconds() - view_at);
        const double budget = move_budget(remaining, increment, max_seconds);
        SearchParams sp = {budget >= 3.0 ? 256 : 128, 2000, seed + (unsigned long long)decisions, budget * 0.6, 0};
        if (remaining < 20) { sp.samples = 32; sp.iterations = 60; sp.seconds = 0.3; }
        GtcfrParams gp = {budget >= 3.0 ? 256 : 128, budget, 1500000, 32, 10, seed + (unsigned long long)decisions, NULL, 0, kappa};
        /* With Stockfish at about 1 ms a call, 256 worlds spread the search too
         * thin to find the opponent's punishing reply: at v32's 21 decisions
         * that lost 3+ points to a top-six team, 256 worlds repeated the losing
         * move 11-13 times, 32 worlds 7-9, Misty (32 worlds) 5-6 */
        if (sf_ready()) gp.samples = 32;
        if (getenv("MIST_SAMPLES")) gp.samples = atoi(getenv("MIST_SAMPLES"));
        if (remaining < 20) { gp.samples = 32; gp.seconds = 0.3; }
        MoveList allowed;
        view_moves(view, us, front.castle, &allowed);
        /* ahead: leave out the moves that would make our pieces stand as they
         * already have twice (the referee's threefold needs that) */
        Move repeats[256]; int nrep = 0;
        const int lead = front_lead(&front, view);
        if (lead >= repeat_lead && allowed.count > 0) {
            const int n = front_repeats(&front, view, &allowed, repeats);
            if (n > 0 && n < allowed.count) {
                gp.avoid = repeats; gp.avoid_count = n; nrep = n;
                fprintf(stderr, "mist: ahead by %d; %d moves would repeat our pieces a third time, left out\n", lead, n);
            }
        }
        /* king guard: our opponent model gives a hidden check (a queen on
         * a4 against e8) a few percent, so the search trades it for a pawn;
         * at v8's three 9-12-ply losses to v7 that check had 3.5-6% and six
         * or more moves had none. Leave out the moves more than `guard`
         * above the safest one, measured on all of P (or 20,000 boards) */
        Move guarded[512];
        double mrisk[256], mleast = 1e9;
        int have_mrisk = 0;
        if ((mguard >= 0 || getenv("MIST_RISK") || getenv("MIST_MATERIAL_DIAG")) && b.count > 0 && allowed.count > 1 && !(forced && *forced)) {
            for (int i = 0; i < allowed.count; ++i) { mrisk[i] = material_risk(&b, allowed.moves[i], mcap); if (mrisk[i] < mleast) mleast = mrisk[i]; }
            have_mrisk = 1;
        }
        double qrisk[256] = {0}, qriskt[256] = {0}, qrisku[256] = {0}, qleast = 1, qleastt = 1;
        int have_qrisk = 0, qout[256];
        memset(qout, 0, sizeof qout);
        const double guards_started = now_seconds();
        if ((qguard >= 0 || getenv("MIST_RISK")) && b.count > 0 && allowed.count > 1 && !(forced && *forced)) {
            for (int i = 0; i < allowed.count; ++i) {
                queen_risks(&b, allowed.moves[i], qcap, guard_temp, qloss, &qrisk[i], &qriskt[i], &qrisku[i]);
                if (qrisk[i] < qleast) qleast = qrisk[i];
                if (qriskt[i] < qleastt) qleastt = qriskt[i];
            }
            have_qrisk = 1;
            if (getenv("MIST_TIMING")) fprintf(stderr, "mist: timing queen guard %.3f s |P| %zu moves %d\n", now_seconds() - guards_started, b.count, allowed.count);
        }
        double prisk[256] = {0}, priskt[256] = {0}, prisku[256] = {0}, pleast = 1, pleastt = 1;
        int have_prisk = 0;
        if ((pguard >= 0 || getenv("MIST_RISK")) && b.count > 0 && allowed.count > 1 && !(forced && *forced)) {
            const double started = now_seconds();
            for (int i = 0; i < allowed.count; ++i) {
                piece_risks(&b, allowed.moves[i], pcap, guard_temp, ploss, qloss, &prisk[i], &priskt[i], &prisku[i]);
                if (prisk[i] < pleast) pleast = prisk[i];
                if (priskt[i] < pleastt) pleastt = priskt[i];
            }
            have_prisk = 1;
            if (getenv("MIST_TIMING")) fprintf(stderr, "mist: timing piece guard %.3f s |P| %zu moves %d\n", now_seconds() - started, b.count, allowed.count);
        }
        /* v14: the queen risk joins the king guard's rule below as one risk
         * (king + qweight x queen) when the queen guard is on; qout only marks
         * what the queen added, for the log */
        const double lam = (qguard >= 0 && have_qrisk) ? qweight : 0.0;
        /* v20: the piece risk joins the same rule as a third term, lamp x piece */
        const double lamp = (pguard >= 0 && have_prisk) ? pguard : 0.0;
        if (guard >= 0 && b.count > 0 && allowed.count > 1 && !(forced && *forced)) {
            /* Two weightings. The fitted one catches a check the belief gives
             * a few percent (the a5 queen check after 1.d4: 3%). The sharper
             * one (weight^temp) keeps only the moves safe where the belief
             * is concentrated: on the regression suite it cut the hidden-
             * check losses 73 -> 63 of 300, and alone it let the a5 check
             * through (two 7-9-ply losses). A move is out when it is above
             * the safest under either; if that leaves nothing, under the
             * sharper one alone. guard_temp 1 is the fitted guard only. */
            double krisk[256], kriskt[256], krisku[256];
            for (int i = 0; i < allowed.count; ++i) king_risks(&b, allowed.moves[i], 20000, guard_temp, &krisk[i], &kriskt[i], &krisku[i]);
            int kqout[256], out[256];
            GuardStats gs;
            guard_rule(allowed.count, krisk, kriskt, krisku, qrisk, qriskt, qrisku, prisk, priskt, prisku, lam, lamp, guard, uguard, out, kqout, &gs);
            const double least = gs.least, leastt = gs.leastt, kleast = gs.kleast, kleastt = gs.kleastt;
            const int dropped = gs.dropped;
            Move gex[256]; int ng = 0;   /* the guards' exclusions; united with the repetition rule's below */
            int mdropped = 0, qdropped = 0, pdropped = 0;
            for (int i = 0; i < allowed.count; ++i) {
                /* what the queen added: out under king + queen, not under the king's alone;
                 * what the piece term added: out under all three, not under king + queen */
                const int kout = kriskt[i] > kleastt + guard || krisk[i] > kleast + guard;
                qout[i] = kqout[i] && !kout;
                qdropped += qout[i];
                if (out[i] && !kqout[i]) {   /* the piece term's own vetoes, by name, for the calibration replays */
                    char u[6]; move_to_uci(allowed.moves[i], u);
                    fprintf(stderr, "mist: piece veto %s\n", u);
                    ++pdropped;
                }
                if (out[i]) gex[ng++] = allowed.moves[i];
                else if (mguard >= 0 && have_mrisk && mrisk[i] > mleast + mguard) { gex[ng++] = allowed.moves[i]; ++mdropped; }
            }
            if (dropped + mdropped > 0 && dropped + mdropped < allowed.count) {
                int yielded = 0;
                const int n = front_exclusions(&allowed, repeats, nrep, gex, ng, guarded, &yielded);
                gp.avoid = guarded; gp.avoid_count = n;
                if (yielded) fprintf(stderr, "mist: the guards and the repetition rule would leave no move; the repetition rule yields\n");
                if (dropped > 0) fprintf(stderr, "mist: king guard left out %d of %d moves (safest %.3f, sharper %.3f)\n", dropped, allowed.count, least, leastt);
                if (qdropped > 0) fprintf(stderr, "mist: queen guard left out %d of %d moves (safest %.3f, sharper %.3f)\n", qdropped, allowed.count, qleast, qleastt);
                if (pdropped > 0) fprintf(stderr, "mist: piece guard left out %d of %d moves (safest %.3f, sharper %.3f)\n", pdropped, allowed.count, pleast, pleastt);
                if (mdropped > 0) fprintf(stderr, "mist: material guard left out %d of %d moves (least %.2f)\n", mdropped, allowed.count, mleast);
            }
        }
        SearchResult r; int ok = 0;
        memset(&r, 0, sizeof r);
        if (forced && *forced) {
            char want[8] = ""; int k = 0;
            while (forced[k] && forced[k] != ',' && k < 7) { want[k] = forced[k]; ++k; }
            forced += forced[k] == ',' ? k + 1 : k;
            for (int i = 0; i < allowed.count && !ok; ++i) { char u[6]; move_to_uci(allowed.moves[i], u); if (strcmp(u, want) == 0) { r.move = allowed.moves[i]; ok = 1; } }
            if (!ok && b.count > 0) {   /* e.g. en passant the view cannot prove */
                MoveList l; generate_moves(&b.items[0], &l);
                for (int i = 0; i < l.count && !ok; ++i) { char u[6]; move_to_uci(l.moves[i], u); if (strcmp(u, want) == 0) { r.move = l.moves[i]; ok = 1; } }
            }
            if (!ok) { fprintf(stderr, "mist: forced move %s not found\n", want); break; }
            if (getenv("MIST_RISK")) {   /* diagnostics: the played move's risk on P, and the least */
                double least = 1;
                for (int i = 0; i < allowed.count; ++i) { const double x = king_risk(&b, allowed.moves[i]); if (x < least) least = x; }
                fprintf(stderr, "mist: played %s risk %.4f least %.4f |P|=%zu%s\n", want, king_risk(&b, r.move), least, b.count, b.overflowed ? "(capped)" : "");
            }
            printf("move %s\n", want); fflush(stdout);
            clock_left = reported - (now_seconds() - view_at) + increment;
            front_play(&front, view, r.move);
            last = r.move; moved = 1; ++decisions;
            continue;
        }
        /* one move the view allows: play it, the clock keeps the budget */
        if (allowed.count == 1) { r.move = allowed.moves[0]; r.our_moves = 1; r.probability = 1.0; ok = 1; }
        /* GT-CFR by default (MIST_SEARCH=twoply for the fixed two-ply search) */
        const int twoply = getenv("MIST_SEARCH") && strcmp(getenv("MIST_SEARCH"), "twoply") == 0;
        if (!ok && b.count > 0) ok = twoply ? search_decide(&b, &sp, &r) : gtcfr_decide(&b, &gp, &r);
        if (!ok) {
            /* last resort: decide on the position we can see */
            Position seen; front_view_position(view, us, front.castle, &seen);
            Belief one = {0}; one.items = &seen; one.count = 1; one.us = us;
            ok = search_decide(&one, &sp, &r);
            fprintf(stderr, "mist: no belief, decided on the visible position\n");
        }
        if (allowed.count > 0) {
            int found = 0;
            for (int i = 0; i < allowed.count; ++i) found |= allowed.moves[i] == r.move;
            if (ok && !found && !b.overflowed) found = legal_everywhere(&b, r.move);
            if (!ok || !found) {
                char bad[6]; move_to_uci(r.move, bad);
                r.move = front_best_allowed_avoiding(&front, view, &allowed, gp.avoid, gp.avoid_count);   /* the guards' exclusions hold here too */
                fprintf(stderr, "mist: %s is not legal on every board the view allows; playing a move it allows\n", ok ? bad : "(none)");
                ok = 1;
            }
        }
        if (!ok) { fprintf(stderr, "mist: no legal move\n"); break; }
        char uci[6]; move_to_uci(r.move, uci);
        double chosen_mrisk = -1, chosen_qrisk = -1, chosen_qriskt = -1;
        if (have_mrisk) for (int i = 0; i < allowed.count; ++i) if (allowed.moves[i] == r.move) chosen_mrisk = mrisk[i];
        if (have_qrisk) for (int i = 0; i < allowed.count; ++i) if (allowed.moves[i] == r.move) { chosen_qrisk = qrisk[i]; chosen_qriskt = qriskt[i]; }
        if (getenv("MIST_RISK")) {   /* diagnostics: each allowed move's king, queen and material risk on P */
            for (int i = 0; i < allowed.count; ++i) {
                char u[6]; move_to_uci(allowed.moves[i], u);
                fprintf(stderr, "mist: risk %s %.4f qrisk %.3f/%.3f/%.3f mrisk %.2f%s\n", u, king_risk(&b, allowed.moves[i]), have_qrisk ? qrisk[i] : -1.0, have_qrisk ? qriskt[i] : -1.0, have_qrisk ? qrisku[i] : -1.0, have_mrisk ? mrisk[i] : -1.0, allowed.moves[i] == r.move ? " (chosen)" : "");
            }
        }
        if (have_qrisk) fprintf(stderr, "mist: queen risk chosen %.3f/%.3f least %.3f/%.3f\n", chosen_qrisk, chosen_qriskt, qleast, qleastt);
        long sf_calls; double sf_seconds;
        sf_stats(&sf_calls, &sf_seconds);
        unsigned long hist_lookups, hist_hits;
        belief_history_stats(&hist_lookups, &hist_hits);
        fprintf(stderr, "mist: n=%d |P|=%zu%s samples=%d moves=%d infosets=%d nodes=%d iters=%d p=%.2f v=%+.3f lead=%+d clock=%.1f sf=%ld/%.1fs mr=%.2f/%.2f hist=%lu/%lu move=%s\n",
                ++decisions, b.count, b.overflowed ? "(capped)" : "", r.samples, r.our_moves, r.opponent_infosets, r.nodes,
                r.iterations, r.probability, r.value, lead, remaining, sf_calls, sf_seconds, chosen_mrisk, have_mrisk ? mleast : -1.0, hist_hits, hist_lookups, uci);
        printf("move %s\n", uci); fflush(stdout);
        clock_left = reported - (now_seconds() - view_at) + increment;
        front_play(&front, view, r.move);
        last = r.move; moved = 1;
        if (forced) break;   /* decision replay: one searched decision */
    }
    belief_free(&b);
    sf_stop();
    return 0;
}
