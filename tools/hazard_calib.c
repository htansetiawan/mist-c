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

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "tests/traces-v35.txt";
    const int max_games = argc > 2 ? atoi(argv[2]) : 1000000;
    const size_t cap = argc > 3 ? (size_t)strtoull(argv[3], NULL, 10) : 200000;
    const size_t cap_eval = argc > 4 ? (size_t)strtoull(argv[4], NULL, 10) : 20000;
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 2; }
    fow_init(); zobrist_init();
    const double temp = belief_temperature();
    printf("game\tply\tside\tmove\tpiece\tcapture_truth\tcapture_seen\tplayed\tP\tcapped\tworlds\tp_fitted\tp_sharp\tp_uniform\ttruth\ttruth_share\tq_fitted\tq_sharp\tq_uniform\ttruth_clean\tdefended_truth\tmin_att_truth\tmaterial_before\tu_fitted\tu_sharp\tu_uniform\ttruth_undefended\n");
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
                for (int k = 0; k < allowed.count; ++k) {
                    const Move c = allowed.moves[k];
                    const int pt = b.items[0].piece[MOVE_FROM(c)];
                    if (pt != KNIGHT && pt != BISHOP && pt != ROOK && pt != QUEEN) continue;
                    const int pv = VALUE[pt];
                    double t1 = 0, l1 = 0, tt = 0, lt = 0, tu = 0, lu = 0, c1 = 0, ct = 0, cu = 0, d1 = 0, dt = 0, du = 0; long worlds = 0;
                    for (size_t i = 0; i < b.count; i += stride) {
                        const double lw = b.logw ? (double)b.logw[i] : 0.0;
                        const double wt = exp(lw), w1 = exp(lw / temp);
                        int ma, df;
                        const int hit = fog_hazard(&b.items[i], c, us, cur, &ma, &df);
                        const int clean = hit && (!df || ma < pv);
                        t1 += w1; tt += wt; tu += 1; ++worlds;
                        if (hit) { l1 += w1; lt += wt; lu += 1; }
                        if (clean) { c1 += w1; ct += wt; cu += 1; }
                        if (hit && !df) { d1 += w1; dt += wt; du += 1; }
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
                    printf("%s\t%ld\t%c\t%s\t%c\t%d\t%d\t%d\t%zu\t%d\t%ld\t%.6f\t%.6f\t%.6f\t%d\t%.6g\t%.6f\t%.6f\t%.6f\t%d\t%d\t%d\t%d\t%.6f\t%.6f\t%.6f\t%d\n", id, ply, us == WHITE ? 'w' : 'b', u, "PNBRQK"[pt],
                           cap_truth, cap_seen, c == m, b.count, b.overflowed, worlds, t1 > 0 ? l1 / t1 : 0, tt > 0 ? lt / tt : 0, tu > 0 ? lu / tu : 0, truth, tshare,
                           t1 > 0 ? c1 / t1 : 0, tt > 0 ? ct / tt : 0, tu > 0 ? cu / tu : 0, truth_clean, df_t, ma_t, mat,
                           t1 > 0 ? d1 / t1 : 0, tt > 0 ? dt / tt : 0, tu > 0 ? du / tu : 0, truth_undef);
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
    fprintf(stderr, "games %d decisions %ld rows %ld in %.0f s (cap %zu, eval stride cap %zu, temp %.2f)\n", games, decisions, rows, now() - t_start, cap, cap_eval, temp);
    return 0;
}
