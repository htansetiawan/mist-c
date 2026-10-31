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
 * Attack re-weighting (31 October 2026). MIST_ATTACK_BOOST adds the same estimates with every
 * world's weight multiplied by exp(lambda * a(w)) before the sums (fitted and sharp alike), for
 * a list of (mode, lambda) combos. The spec is "mode:l1,l2;mode:l1,..."; a bare list "l1,l2"
 * means the modes all and und with those lambdas. a(w), always from the position before our
 * move, with "unseen" meaning the enemy piece stands on a '?' square of our view:
 *   all    our knights, bishops, rooks and queens attacked by an unseen enemy piece
 *   und    the same, counting only those no piece of ours defends
 *   dest   distinct destination squares of our legal moves (from the view, so the same set
 *          in every world) attacked by an unseen enemy piece
 *   cover  distinct squares anywhere on the board attacked by unseen enemy pieces
 *   destn  dest divided by the number of destination squares
 * Columns per combo: p (attacked), q (clean one-exchange loss), u (attacked and undefended),
 * each under the fitted and the sharp weighting. A "D" line per decision gives, after game,
 * ply, |P|, capped and the number of entries of the true board: for each of the five modes
 * a(truth), the belief's sharp and fitted expectation of a; then the true board's weight share
 * and rank in the whole of P under the plain weights and under every combo. Unset, the output
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

enum { MODE_ALL, MODE_UND, MODE_DEST, MODE_COVER, MODE_DESTN, NMODES };
static const char *MODE_NAME[NMODES] = {"all", "und", "dest", "cover", "destn"};

/* The per-state features of a world (see the header): all, und, dest and cover. */
static void fog_features(const Position *p, int us, const char view[65], bb_t destbb, int *all, int *und, int *dest, int *cover) {
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
    *dest = __builtin_popcountll(destbb & att);
    *cover = __builtin_popcountll(att);
}

#define MAX_COMBO 32

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "tests/traces-v35.txt";
    const int max_games = argc > 2 ? atoi(argv[2]) : 1000000;
    const size_t cap = argc > 3 ? (size_t)strtoull(argv[3], NULL, 10) : 200000;
    const size_t cap_eval = argc > 4 ? (size_t)strtoull(argv[4], NULL, 10) : 20000;
    /* re-weighting sweep: combos (mode, lambda) */
    int cmode[MAX_COMBO]; double lambda[MAX_COMBO]; int nc = 0;
    if (getenv("MIST_ATTACK_BOOST")) {
        char spec[512]; strncpy(spec, getenv("MIST_ATTACK_BOOST"), sizeof spec - 1); spec[sizeof spec - 1] = 0;
        if (strchr(spec, ':')) {
            char *save1 = NULL;
            for (char *seg = strtok_r(spec, ";", &save1); seg; seg = strtok_r(NULL, ";", &save1)) {
                char *colon = strchr(seg, ':'); if (!colon) continue;
                *colon = 0; int mode = -1;
                for (int m = 0; m < NMODES; ++m) if (strcmp(seg, MODE_NAME[m]) == 0) mode = m;
                if (mode < 0) { fprintf(stderr, "MIST_ATTACK_BOOST: unknown mode %s\n", seg); return 2; }
                char *save2 = NULL;
                for (char *tok = strtok_r(colon + 1, ", ", &save2); tok && nc < MAX_COMBO; tok = strtok_r(NULL, ", ", &save2)) { cmode[nc] = mode; lambda[nc++] = atof(tok); }
            }
        } else {
            double ls[MAX_COMBO / 2]; int nl = 0;
            for (char *tok = strtok(spec, ", "); tok && nl < MAX_COMBO / 2; tok = strtok(NULL, ", ")) ls[nl++] = atof(tok);
            for (int m = MODE_ALL; m <= MODE_UND; ++m) for (int j = 0; j < nl; ++j) { cmode[nc] = m; lambda[nc++] = ls[j]; }
        }
    }
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 2; }
    fow_init(); zobrist_init();
    const double temp = belief_temperature();
    printf("game\tply\tside\tmove\tpiece\tcapture_truth\tcapture_seen\tplayed\tP\tcapped\tworlds\tp_fitted\tp_sharp\tp_uniform\ttruth\ttruth_share\tq_fitted\tq_sharp\tq_uniform\ttruth_clean\tdefended_truth\tmin_att_truth\tmaterial_before\tu_fitted\tu_sharp\tu_uniform\ttruth_undefended");
    for (int c = 0; c < nc; ++c) {
        const char *v = MODE_NAME[cmode[c]]; const double l = lambda[c];
        printf("\tp_fitted_%s_%g\tp_sharp_%s_%g\tq_fitted_%s_%g\tq_sharp_%s_%g\tu_fitted_%s_%g\tu_sharp_%s_%g", v, l, v, l, v, l, v, l, v, l, v, l);
    }
    printf("\n");
    /* per-world features over all of P, and per-combo weights of the strided worlds */
    unsigned char *feat[4] = {NULL, NULL, NULL, NULL}; size_t a_cap = 0;
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
            have = 1; ++games; skip = 0; ply = 0; have_view = 0;   /* a stale view must not label the next game's first reply */
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
                    bb_t destbb = 0; for (int k = 0; k < allowed.count; ++k) destbb |= 1ULL << MOVE_TO(allowed.moves[k]);
                    const double n_dest = __builtin_popcountll(destbb) ? (double)__builtin_popcountll(destbb) : 1.0;
                    if (b.count > a_cap) { a_cap = b.count; for (int k = 0; k < 4; ++k) feat[k] = realloc(feat[k], a_cap); }
                    if (b.count > l_cap) { l_cap = b.count; L = realloc(L, l_cap * sizeof(double)); }
                    for (size_t i = 0; i < b.count; ++i) {
                        int v[4]; fog_features(&b.items[i], us, cur, destbb, &v[0], &v[1], &v[2], &v[3]);
                        for (int k = 0; k < 4; ++k) feat[k][i] = (unsigned char)(v[k] > 255 ? 255 : v[k]);
                    }
#define FEAT(mode, i) ((mode) == MODE_DESTN ? feat[MODE_DEST][i] / n_dest : (double)feat[mode][i])
                    for (size_t i = 0; i < b.count; i += stride, ++nstr) {
                        const double lw = b.logw ? (double)b.logw[i] : 0.0;
                        for (int c = 0; c < nc; ++c) {
                            const double la = lambda[c] * FEAT(cmode[c], i);
                            W1[(size_t)c * (cap_eval + 2) + nstr] = exp(lw / temp + la);
                            WT[(size_t)c * (cap_eval + 2) + nstr] = exp(lw + la);
                        }
                    }
                    /* the true board in P: features, then share and rank under the plain weights and under every combo */
                    const uint64_t key = position_key(&truth_before);
                    int vt[4]; fog_features(&truth_before, us, cur, destbb, &vt[0], &vt[1], &vt[2], &vt[3]);
                    double es[NMODES] = {0}, ef[NMODES] = {0}, ts = 0, tf = 0;
                    size_t truth_n = 0;
                    for (size_t i = 0; i < b.count; ++i) {
                        const double lw = b.logw ? (double)b.logw[i] : 0.0, ws = exp(lw), wf = exp(lw / temp);
                        ts += ws; tf += wf;
                        for (int md = 0; md < NMODES; ++md) { const double x = FEAT(md, i); es[md] += ws * x; ef[md] += wf * x; }
                        if (position_key(&b.items[i]) == key) ++truth_n;
                    }
                    printf("D\t%s\t%ld\t%zu\t%d\t%zu", id, ply, b.count, b.overflowed, truth_n);
                    for (int md = 0; md < NMODES; ++md) {
                        const double at = md == MODE_DESTN ? vt[MODE_DEST] / n_dest : (double)vt[md];
                        printf("\t%.4g\t%.4f\t%.4f", at, es[md] / ts, ef[md] / tf);
                    }
                    for (int c = -1; c < nc; ++c) {
                        double mx = -1e300, tmax = -1e300;
                        for (size_t i = 0; i < b.count; ++i) {
                            const double lw = b.logw ? (double)b.logw[i] : 0.0;
                            L[i] = c < 0 ? lw : lw + lambda[c] * FEAT(cmode[c], i);
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
#undef FEAT
                }
                for (int k = 0; k < allowed.count; ++k) {
                    const Move c = allowed.moves[k];
                    const int pt = b.items[0].piece[MOVE_FROM(c)];
                    if (pt != KNIGHT && pt != BISHOP && pt != ROOK && pt != QUEEN) continue;
                    const int pv = VALUE[pt];
                    double t1 = 0, l1 = 0, tt = 0, lt = 0, tu = 0, lu = 0, c1 = 0, ct = 0, cu = 0, d1 = 0, dt = 0, du = 0; long worlds = 0;
                    double T1[MAX_COMBO] = {0}, TT[MAX_COMBO] = {0}, L1[MAX_COMBO] = {0}, LT[MAX_COMBO] = {0},
                           C1[MAX_COMBO] = {0}, CT[MAX_COMBO] = {0}, D1[MAX_COMBO] = {0}, DT[MAX_COMBO] = {0};
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
    for (int k = 0; k < 4; ++k) free(feat[k]);
    free(W1); free(WT); free(L);
    fprintf(stderr, "games %d decisions %ld rows %ld in %.0f s (cap %zu, eval stride cap %zu, temp %.2f, boost combos %d)\n", games, decisions, rows, now() - t_start, cap, cap_eval, temp, nc);
    return 0;
}
