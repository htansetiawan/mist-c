/* belief_keep_board (the oracle's collapse, main.c MIST_ORACLE_FILE): keeps
 * the worlds with the given placement, places the fallback when none is left. */
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
    if (fails) printf("test_oracle: %d failures\n", fails); else printf("test_oracle: ok\n");
    return fails != 0;
}
