/* Fog-hazard calibration of the belief (30 October 2026).
 *
 * Replays a traces file with the belief exactly as tests/test_belief.c does and, at every one
 * of OUR decisions with |P| > 1, scores every candidate move of a knight, bishop, rook or queen
 * (the moves legal on every board the view allows, view_moves, as the engine's guards see
 * them) by the belief's probability that the destination is attacked by an enemy piece
 * standing on a square we cannot see, under three weightings of P strided to at most `cap_eval`
 * boards (the guards' 20,000): the fitted weights exp(logw / temp), the sharper weights
 * exp(logw) the search samples by, and uniform. The truth label is the same test on the true
 * board. One TSV row per (decision, move) on stdout; progress on stderr.
 *     MIST_THREADS=1 MIST_HISTORY_TABLE=... ./build/hazard_calib TRACES [games] [cap=200000] [cap_eval=20000]
 *
 * Attack re-weighting (31 October 2026). MIST_ATTACK_BOOST=l1,l2,... (a list of lambdas)
 * adds, for each lambda and for two counts a(w) of a world w, the same estimates with every
 * world's weight multiplied by exp(lambda * a(w)) before the sums (fitted and sharp alike):
 *   all   a(w) = our knights, bishops, rooks and queens attacked in w by an enemy piece
 *                standing on a square that is '?' in our view
 *   und   the same, counting only those no piece of ours defends
 * Columns per (count, lambda): p (attacked), q (clean one-exchange loss), u (attacked and
 * undefended), each under the fitted and the sharp weighting. A "D" line per decision gives
 * the true board's weight share and rank in the whole of P under the plain weights and under
 * every (count, lambda), plus a(truth) and the belief's expectation of a. Unset, the output
 * is the 30 October format, byte for byte.
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/belief.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static const int VALUE[6] = {1, 3, 3, 5, 9, 0};

/* After `m` on a copy of `p0`: attacked = the destination is attacked by an enemy piece standing on a square
 * that is '?' in `view`; *min_att = the cheapest such attacker's value (0 if none); *defended = one of our
 * pieces attacks the destination afterwards. clean = attacked and (undefended or the attacker is worth less
 * than the moved piece): a one-exchange estimate of losing material, the shape piece_risks in main.c uses. */
static int fog_hazard(const Position *p0, Move m, int us, const char view[65], int *min_att, int *defended) {
    Position p = *p0; Undo u;
    make_move(&p, m, &u);
    const int to = MOVE_TO(m);
    int best = 0;
    bb_t enemy = p.by_color[!us];
    while (enemy) {
        const int s = __builtin_ctzll(enemy); enemy &= enemy - 1;
        if (view[s] != '?') continue;
        if (attacks_from(&p, s) & (1ULL << to)) { const int v = VALUE[p.piece[s]] ? VALUE[p.piece[s]] : 100; if (!best || v < best) best = v; }
    }
    *min_att = best;
    *defended = is_attacked(&p, to, us);
    return best > 0;
}

/* a(w): our knights, bishops, rooks and queens attacked by an enemy piece standing on a '?' square of
 * `view`; *und = those that no piece of ours defends. */
static void fog_attacks(const Position *p, int us, const char view[65], int *all, int *und) {
    bb_t att = 0, enemy = p->by_color[!us];
    while (enemy) {
        const int s = __builtin_ctzll(enemy); enemy &= enemy - 1;
        if (view[s] == '?') att |= attacks_from(p, s);
    }
    bb_t hit = p->by_color[us] & (p->by_type[KNIGHT] | p->by_type[BISHOP] | p->by_type[ROOK] | p->by_type[QUEEN]) & att;
    *all = __builtin_popcountll(hit);
    int u = 0;
    while (hit) { const int sq = __builtin_ctzll(hit); hit &= hit - 1; if (!is_attacked(p, sq, us)) ++u; }
    *und = u;
}

#define MAX_LAMBDA 16

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "tests/traces-v35.txt";
    const int max_games = argc > 2 ? atoi(argv[2]) : 1000000;
    const size_t cap = argc > 3 ? (size_t)strtoull(argv[3], NULL, 10) : 200000;
    const size_t cap_eval = argc > 4 ? (size_t)strtoull(argv[4], NULL, 10) : 20000;
    /* re-weighting sweep: combos c = variant * nl + j, variant 0 = all, 1 = undefended */
    double lambda[MAX_LAMBDA]; int nl = 0;
    if (getenv("MIST_ATTACK_BOOST")) {
        char spec[256]; strncpy(spec, getenv("MIST_ATTACK_BOOST"), sizeof spec - 1); spec[sizeof spec - 1] = 0;
        for (char *tok = strtok(spec, ", "); tok && nl < MAX_LAMBDA; tok = strtok(NULL, ", ")) lambda[nl++] = atof(tok);
    }
    const int nc = 2 * nl;
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 2; }
    fow_init(); zobrist_init();
    const double temp = belief_temperature();
    printf("game\tply\tside\tmove\tpiece\tcapture_truth\tcapture_seen\tplayed\tP\tcapped\tworlds\tp_fitted\tp_sharp\tp_uniform\ttruth\ttruth_share\tq_fitted\tq_sharp\tq_uniform\ttruth_clean\tdefended_truth\tmin_att_truth\tmaterial_before\tu_fitted\tu_sharp\tu_uniform\ttruth_undefended");
    for (int c = 0; c < nc; ++c) {
        const char *v = c < nl ? "all" : "und"; const double l = lambda[c % nl];
        printf("\tp_fitted_%s_%g\tp_sharp_%s_%g\tq_fitted_%s_%g\tq_sharp_%s_%g\tu_fitted_%s_%g\tu_sharp_%s_%g", v, l, v, l, v, l, v, l, v, l, v, l);
    }
    printf("\n");
    /* per-world attack counts over all of P, and per-combo weights of the strided worlds */
    unsigned char *a_all = NULL, *a_und = NULL; size_t a_cap = 0;
    double *W1 = nc ? malloc((size_t)nc * (cap_eval + 2) * sizeof(double)) : NULL;
    double *WT = nc ? malloc((size_t)nc * (cap_eval + 2) * sizeof(double)) : NULL;
    double *L = NULL; size_t l_cap = 0;   /* logits of all of P for one combo */
    char line[512], id[64] = "";
    Belief b = {0}; Position truth_before; int have = 0, games = 0, skip = 0, us = WHITE; long ply = 0, decisions = 0, rows = 0;
    char last_view[65] = ""; int have_view = 0;
    const double t_start = now();
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "game ", 5) == 0) {
            char side[4];
            if (games >= max_games) break;
            sscanf(line + 5, "%63s %3s", id, side);
            if (have) belief_free(&b);
            us = side[0] == 'w' ? WHITE : BLACK;
            belief_init_start(&b, us, cap);
            pos_start(&truth_before);
            have = 1; ++games; skip = 0; ply = 0;
            if (games % 20 == 0) fprintf(stderr, "  game %d, %ld decisions, %ld rows, %.0f s\n", games, decisions, rows, now() - t_start);
            continue;
        }
        if (skip) continue;
        char who[8], uci[8], view[65], board[65];
        int side, ep; uint64_t castle;
        if (sscanf(line, "%7s %7s %64s %64s %d %" SCNu64 " %d", who, uci, view, board, &side, &castle, &ep) != 7) continue;
        ++ply;
        if (strcmp(who, "us") == 0) {
            Move m = 0; MoveList l; generate_moves(&b.items[0], &l);
            for (int i = 0; i < l.count; ++i) { char u[6]; move_to_uci(l.moves[i], u); if (strcmp(u, uci) == 0) { m = l.moves[i]; break; } }
            if (b.count > 1 && m) {
                ++decisions;
                char cur[65]; observation(&b.items[0], us, cur);          /* every world shares our view */
                memcpy(last_view, cur, 65); have_view = 1;
                const bb_t our_rank = us == WHITE ? 0xFFULL : 0xFF00000000000000ULL;
                MoveList allowed; view_moves(cur, us, b.items[0].castle_rooks & our_rank, &allowed);
                const double tshare = belief_share(&b, &truth_before);
                const size_t stride = b.count > cap_eval ? (b.count + cap_eval - 1) / cap_eval : 1;
                size_t nstr = 0;
                if (nc) {
                    if (b.count > a_cap) { a_cap = b.count; a_all = realloc(a_all, a_cap); a_und = realloc(a_und, a_cap); }
                    if (b.count > l_cap) { l_cap = b.count; L = realloc(L, l_cap * sizeof(double)); }
                    for (size_t i = 0; i < b.count; ++i) { int aa, au; fog_attacks(&b.items[i], us, cur, &aa, &au); a_all[i] = (unsigned char)(aa > 255 ? 255 : aa); a_und[i] = (unsigned char)au; }
                    for (size_t i = 0; i < b.count; i += stride, ++nstr) {
                        const double lw = b.logw ? (double)b.logw[i] : 0.0;
                        for (int c = 0; c < nc; ++c) {
                            const double la = lambda[c % nl] * (c < nl ? a_all[i] : a_und[i]);
                            W1[(size_t)c * (cap_eval + 2) + nstr] = exp(lw / temp + la);
                            WT[(size_t)c * (cap_eval + 2) + nstr] = exp(lw + la);
                        }
                    }
                    /* the true board in P: share and rank under the plain weights and under every combo */
                    const uint64_t key = position_key(&truth_before);
                    int aa_t, au_t; fog_attacks(&truth_before, us, cur, &aa_t, &au_t);
                    double ea_s = 0, eu_s = 0, ea_f = 0, eu_f = 0, ts = 0, tf = 0;
                    size_t truth_n = 0;
                    for (size_t i = 0; i < b.count; ++i) {
                        const double lw = b.logw ? (double)b.logw[i] : 0.0, ws = exp(lw), wf = exp(lw / temp);
                        ts += ws; tf += wf; ea_s += ws * a_all[i]; eu_s += ws * a_und[i]; ea_f += wf * a_all[i]; eu_f += wf * a_und[i];
                        if (position_key(&b.items[i]) == key) ++truth_n;
                    }
                    printf("D\t%s\t%ld\t%zu\t%d\t%zu\t%d\t%d\t%.4f\t%.4f\t%.4f\t%.4f", id, ply, b.count, b.overflowed, truth_n, aa_t, au_t, ea_s / ts, eu_s / ts, ea_f / tf, eu_f / tf);
                    for (int c = -1; c < nc; ++c) {
                        double mx = -1e300, tmax = -1e300;
                        for (size_t i = 0; i < b.count; ++i) {
                            const double lw = b.logw ? (double)b.logw[i] : 0.0;
                            L[i] = c < 0 ? lw : lw + lambda[c % nl] * (c < nl ? a_all[i] : a_und[i]);
                            if (L[i] > mx) mx = L[i];
                        }
                        double total = 0, mine = 0; long above = 0;
                        if (truth_n) for (size_t i = 0; i < b.count; ++i) if (position_key(&b.items[i]) == key && L[i] > tmax) tmax = L[i];
                        for (size_t i = 0; i < b.count; ++i) {
                            const double w = exp(L[i] - mx);
                            total += w;
                            const int is_t = truth_n && position_key(&b.items[i]) == key;
                            if (is_t) mine += w; else if (L[i] > tmax) ++above;
                        }
                        printf("\t%.6g\t%ld", truth_n ? mine / total : 0.0, truth_n ? above + 1 : 0L);
                    }
                    printf("\n");
                }
                for (int k = 0; k < allowed.count; ++k) {
                    const Move c = allowed.moves[k];
                    const int pt = b.items[0].piece[MOVE_FROM(c)];
                    if (pt != KNIGHT && pt != BISHOP && pt != ROOK && pt != QUEEN) continue;
                    const int pv = VALUE[pt];
                    double t1 = 0, l1 = 0, tt = 0, lt = 0, tu = 0, lu = 0, c1 = 0, ct = 0, cu = 0, d1 = 0, dt = 0, du = 0; long worlds = 0;
                    double T1[2 * MAX_LAMBDA] = {0}, TT[2 * MAX_LAMBDA] = {0}, L1[2 * MAX_LAMBDA] = {0}, LT[2 * MAX_LAMBDA] = {0},
                           C1[2 * MAX_LAMBDA] = {0}, CT[2 * MAX_LAMBDA] = {0}, D1[2 * MAX_LAMBDA] = {0}, DT[2 * MAX_LAMBDA] = {0};
                    size_t j = 0;
                    for (size_t i = 0; i < b.count; i += stride, ++j) {
                        const double lw = b.logw ? (double)b.logw[i] : 0.0;
                        const double wt = exp(lw), w1 = exp(lw / temp);
                        int ma, df;
                        const int hit = fog_hazard(&b.items[i], c, us, cur, &ma, &df);
                        const int clean = hit && (!df || ma < pv);
                        t1 += w1; tt += wt; tu += 1; ++worlds;
                        if (hit) { l1 += w1; lt += wt; lu += 1; }
                        if (clean) { c1 += w1; ct += wt; cu += 1; }
                        if (hit && !df) { d1 += w1; dt += wt; du += 1; }
                        for (int cc = 0; cc < nc; ++cc) {
                            const double x1 = W1[(size_t)cc * (cap_eval + 2) + j], xt = WT[(size_t)cc * (cap_eval + 2) + j];
                            T1[cc] += x1; TT[cc] += xt;
                            if (hit) { L1[cc] += x1; LT[cc] += xt; }
                            if (clean) { C1[cc] += x1; CT[cc] += xt; }
                            if (hit && !df) { D1[cc] += x1; DT[cc] += xt; }
                        }
                    }
                    int ma_t, df_t;
                    const int truth = fog_hazard(&truth_before, c, us, cur, &ma_t, &df_t);
                    const int truth_clean = truth && (!df_t || ma_t < pv);
                    const int truth_undef = truth && !df_t;
                    const int to = MOVE_TO(c);
                    const int cap_truth = truth_before.color[to] == !us;
                    const int cap_seen = cur[to] != '?' && cur[to] != '-';
                    int mat = 0; for (int sq = 0; sq < 64; ++sq) if (truth_before.piece[sq] != NO_PIECE) mat += (truth_before.color[sq] == us ? 1 : -1) * VALUE[truth_before.piece[sq]];
                    char u[6]; move_to_uci(c, u);
                    printf("%s\t%ld\t%c\t%s\t%c\t%d\t%d\t%d\t%zu\t%d\t%ld\t%.6f\t%.6f\t%.6f\t%d\t%.6g\t%.6f\t%.6f\t%.6f\t%d\t%d\t%d\t%d\t%.6f\t%.6f\t%.6f\t%d", id, ply, us == WHITE ? 'w' : 'b', u, "PNBRQK"[pt],
                           cap_truth, cap_seen, c == m, b.count, b.overflowed, worlds, t1 > 0 ? l1 / t1 : 0, tt > 0 ? lt / tt : 0, tu > 0 ? lu / tu : 0, truth, tshare,
                           t1 > 0 ? c1 / t1 : 0, tt > 0 ? ct / tt : 0, tu > 0 ? cu / tu : 0, truth_clean, df_t, ma_t, mat,
                           t1 > 0 ? d1 / t1 : 0, tt > 0 ? dt / tt : 0, tu > 0 ? du / tu : 0, truth_undef);
                    for (int cc = 0; cc < nc; ++cc)
                        printf("\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f", T1[cc] > 0 ? L1[cc] / T1[cc] : 0, TT[cc] > 0 ? LT[cc] / TT[cc] : 0,
                               T1[cc] > 0 ? C1[cc] / T1[cc] : 0, TT[cc] > 0 ? CT[cc] / TT[cc] : 0, T1[cc] > 0 ? D1[cc] / T1[cc] : 0, TT[cc] > 0 ? DT[cc] / TT[cc] : 0);
                    printf("\n");
                    ++rows;
                }
            }
            belief_after_own(&b, m, view);
        } else {
            if (have_view && strlen(uci) >= 4) {
                const int from = (uci[0] - 'a') + 8 * (uci[1] - '1');
                printf("R\t%s\t%ld\t%s\t%d\n", id, ply, uci, last_view[from] == '?');
            }
            have_view = 0;
            belief_after_opponent(&b, view);
        }
        pos_from_board_string(&truth_before, board, side, castle, ep);
        { int mat = 0; for (int sq = 0; sq < 64; ++sq) if (truth_before.piece[sq] != NO_PIECE) mat += (truth_before.color[sq] == us ? 1 : -1) * VALUE[truth_before.piece[sq]];
          printf("M\t%s\t%ld\t%d\t%s\t%s\n", id, ply, mat, who, uci); }
        if (b.count == 0) skip = 1;
    }
    if (have) belief_free(&b);
    free(a_all); free(a_und); free(W1); free(WT); free(L);
    fprintf(stderr, "games %d decisions %ld rows %ld in %.0f s (cap %zu, eval stride cap %zu, temp %.2f, boost lambdas %d)\n", games, decisions, rows, now() - t_start, cap, cap_eval, temp, nl);
    return 0;
}
