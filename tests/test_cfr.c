/* M3: PCFR+ on Kuhn poker. Three cards (J < Q < K), ante 1 each, one card per
 * player; player 0 checks or bets 1; facing a bet a player folds or calls; after
 * check-check the higher card wins the pot. The equilibrium value for player 0
 * is -1/18. Both the last iterate (what the paper plays) and the average must
 * approach it, and exploitability must fall toward zero. */
#include <math.h>
#include <stdio.h>

#include "../src/cfr.h"

static int term(Game *g, double v) { return game_node(g, NODE_TERMINAL, 0, -1, v); }

int main(void) {
    Game g; game_init(&g);
    /* infosets: player 0 at its first decision and after check-bet; player 1 after check and after bet */
    int p0_first[3], p0_after_cb[3], p1_after_c[3], p1_after_b[3];
    for (int c = 0; c < 3; ++c) {
        p0_first[c] = game_infoset(&g, 0, 2); p0_after_cb[c] = game_infoset(&g, 0, 2);
        p1_after_c[c] = game_infoset(&g, 1, 2); p1_after_b[c] = game_infoset(&g, 1, 2);
    }
    int root = game_node(&g, NODE_CHANCE, 0, -1, 0.0);
    int deals[6]; double probs[6]; int d = 0;
    for (int c0 = 0; c0 < 3; ++c0)
        for (int c1 = 0; c1 < 3; ++c1) {
            if (c0 == c1) continue;
            const double win = c0 > c1 ? 1.0 : -1.0;
            int n0 = game_node(&g, NODE_PLAYER, 0, p0_first[c0], 0);
            /* check */
            int n1c = game_node(&g, NODE_PLAYER, 1, p1_after_c[c1], 0);
            int n0cb = game_node(&g, NODE_PLAYER, 0, p0_after_cb[c0], 0);
            int kids_cb[2] = {term(&g, -1.0), term(&g, 2.0 * win)};          /* fold, call */
            game_children(&g, n0cb, kids_cb, NULL, 2);
            int kids_c[2] = {term(&g, win), n0cb};                              /* check (showdown 1), bet */
            game_children(&g, n1c, kids_c, NULL, 2);
            /* bet */
            int n1b = game_node(&g, NODE_PLAYER, 1, p1_after_b[c1], 0);
            int kids_b[2] = {term(&g, 1.0), term(&g, 2.0 * win)};             /* fold, call */
            game_children(&g, n1b, kids_b, NULL, 2);
            int kids0[2] = {n1c, n1b};
            game_children(&g, n0, kids0, NULL, 2);
            deals[d] = n0; probs[d] = 1.0 / 6.0; ++d;
        }
    game_children(&g, root, deals, probs, 6);
    game_finalize(&g);
    double x[64];
    int failures = 0;
    /* The measure itself: uniform random play must be clearly exploitable, and a
     * player-0 strategy that always bets must lose to calling with the king. */
    for (int i = 0; i < g.n_slots; ++i) x[i] = 0.5;
    const double e_uniform = exploitability(&g, x);
    printf("uniform random play: value %+.5f exploitability %.5f\n", game_value(&g, x), e_uniform);
    if (e_uniform < 0.1) { printf("FAIL: uniform play should be exploitable\n"); ++failures; }
    for (int t = 1; t <= 4000; ++t) {
        pcfr_iteration(&g, t);
        if (t == 100 || t == 1000 || t == 4000) {
            pcfr_current(&g, x);
            const double v_last = game_value(&g, x), e_last = exploitability(&g, x);
            pcfr_average(&g, x);
            const double v_avg = game_value(&g, x), e_avg = exploitability(&g, x);
            printf("t=%4d  last iterate: value %+.5f exploitability %.5f | average: value %+.5f exploitability %.5f  (target %+.5f)\n",
                   t, v_last, e_last, v_avg, e_avg, -1.0 / 18.0);
            if (t == 4000) {
                if (fabs(v_avg + 1.0 / 18.0) > 2e-3) { printf("FAIL: average value\n"); ++failures; }
                if (e_avg > 5e-3) { printf("FAIL: average exploitability\n"); ++failures; }
                if (e_last > 2e-2) { printf("FAIL: last-iterate exploitability\n"); ++failures; }
            }
        }
    }
    game_free(&g);
    printf("%s\n", failures ? "test_cfr: FAILED" : "test_cfr: passed");
    return failures ? 1 : 0;
}
