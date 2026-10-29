/* belief_keep_board (the oracle's collapse, main.c MIST_ORACLE_FILE): keeps
 * the worlds with the given placement, places the fallback when none is left. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/belief.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main(void) {
    fow_init();
    zobrist_init();
    const char *start = "RNBQKBNRPPPPPPPP--------------------------------pppppppprnbqkbnr";
    const char *e4    = "RNBQKBNRPPPP-PPP------------P-------------------pppppppprnbqkbnr";
    Belief b;
    belief_init_start(&b, BLACK, 200000);
    CHECK(b.count == 1);
    /* the start placement matches the start belief: kept */
    CHECK(belief_keep_board(&b, start, NULL) == 1 && b.count == 1);
    /* a placement not in P: the fallback becomes the only world, with its castling and en passant */
    Position after;
    CHECK(pos_from_board_string(&after, e4, BLACK, (1ULL << 0) | (1ULL << 7) | (1ULL << 56) | (1ULL << 63), -1) == 0);
    CHECK(belief_keep_board(&b, e4, &after) == 0 && b.count == 1);
    CHECK(position_key(&b.items[0]) == position_key(&after));
    char text[65];
    pos_to_board_string(&b.items[0], text);
    CHECK(strcmp(text, e4) == 0);
    CHECK(b.items[0].side == BLACK);
    /* no match and no fallback: P empties (main.c rebuilds from the view) */
    CHECK(belief_keep_board(&b, start, NULL) == 0 && b.count == 0);
    /* and the fallback fills an emptied P again */
    CHECK(belief_keep_board(&b, e4, &after) == 0 && b.count == 1);
    CHECK(pos_matches_board_string(&b.items[0], e4) && !pos_matches_board_string(&b.items[0], start));
    belief_free(&b);
    /* truth share forcing (MIST_ORACLE_SHARE): the start world and an appended e4 world */
    Belief s;
    belief_init_start(&s, BLACK, 200000);
    double nat = -1, got = -1;
    /* share 0 only measures: the start placement holds all of P */
    CHECK(belief_force_board_share(&s, start, NULL, 0, &nat) == 1 && nat == 1.0 && s.count == 1);
    /* a placement not in P, measured: nothing changes */
    CHECK(belief_force_board_share(&s, e4, NULL, 0, &nat) == 0 && nat == 0.0 && s.count == 1);
    /* forced to 0.2 with the fallback: appended, and it holds 0.2 of the weight */
    CHECK(belief_force_board_share(&s, e4, &after, 0.2, &nat) == 0 && nat == 0.0 && s.count == 2);
    belief_force_board_share(&s, e4, NULL, 0, &got);
    CHECK(fabs(got - 0.2) < 1e-6);
    /* forced again to 0.5: the existing world is rescaled, |P| unchanged */
    CHECK(belief_force_board_share(&s, e4, &after, 0.5, &nat) == 1 && fabs(nat - 0.2) < 1e-6 && s.count == 2);
    belief_force_board_share(&s, e4, NULL, 0, &got);
    CHECK(fabs(got - 0.5) < 1e-6);
    belief_force_board_share(&s, start, NULL, 0, &got);
    CHECK(fabs(got - 0.5) < 1e-6);
    /* and down to 0.05: the sampler sees that share (all of P when |P| <= K) */
    belief_force_board_share(&s, e4, &after, 0.05, &nat);
    int idx[4]; double prob[4]; uint64_t rng = 1;
    CHECK(belief_sample(&s, 4, &rng, idx, prob) == 2);
    for (int i = 0; i < 2; ++i) {
        const double want = pos_matches_board_string(&s.items[idx[i]], e4) ? 0.05 : 0.95;
        CHECK(fabs(prob[i] - want) < 1e-6);
    }
    belief_free(&s);
    if (fails) printf("test_oracle: %d failures\n", fails); else printf("test_oracle: ok\n");
    return fails != 0;
}
