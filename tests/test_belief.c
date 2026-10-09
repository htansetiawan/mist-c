/* M2: exact belief on real arena games. The true position must be in P after
 * every ply (soundness); reports |P| and update time. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/belief.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "tests/traces.txt";
    size_t cap = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : 2000000;
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 2; }
    fow_init();
    zobrist_init();
    char line[512];
    Belief b = {0};
    int have = 0, games = 0;
    long plies = 0, unsound = 0, overflow_games = 0, lost_after_cap = 0;
    int skip = 0;
    size_t max_p = 0; double sum_p = 0, max_t = 0, sum_t = 0;
    int game_overflowed = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "game ", 5) == 0) {
            char id[64], side[4];
            sscanf(line + 5, "%63s %3s", id, side);
            if (have) { overflow_games += game_overflowed; belief_free(&b); }
            belief_init_start(&b, side[0] == 'w' ? WHITE : BLACK, cap);
            have = 1; ++games; game_overflowed = 0; skip = 0;
            continue;
        }
        if (skip) continue;
        char who[8], uci[8], view[65], board[65];
        int side, ep; uint64_t castle;
        if (sscanf(line, "%7s %7s %64s %64s %d %" SCNu64 " %d", who, uci, view, board, &side, &castle, &ep) != 7) continue;
        double t0 = now();
        if (strcmp(who, "us") == 0) {
            /* find our move among the legal moves of any position in P (all share our pieces) */
            Move m = 0; MoveList l; generate_moves(&b.items[0], &l);
            for (int i = 0; i < l.count; ++i) { char u[6]; move_to_uci(l.moves[i], u); if (strcmp(u, uci) == 0) { m = l.moves[i]; break; } }
            belief_after_own(&b, m, view);
        } else {
            belief_after_opponent(&b, view);
        }
        double dt = now() - t0;
        if (getenv("FOW_VERBOSE")) fprintf(stderr, "    %s %s -> |P| %zu in %.3f s%s\n", who, uci, b.count, dt, b.overflowed ? " (capped)" : "");
        Position truth;
        pos_from_board_string(&truth, board, side, castle, ep);
        ++plies;
        if (b.overflowed) game_overflowed = 1;
        if (!belief_contains(&b, &truth)) {
            /* Dropping positions past the cap may drop the truth (expected);
             * losing it while P is still exact is a rules or update bug. */
            if (game_overflowed) ++lost_after_cap;
            else if (unsound++ < 5) fprintf(stderr, "UNSOUND: true position missing after %s %s in game %d (|P| = %zu)\n", who, uci, games, b.count);
        }
        if (b.count > max_p) max_p = b.count;
        sum_p += (double)b.count; sum_t += dt; if (dt > max_t) max_t = dt;
        if (b.count == 0) {
            fprintf(stderr, "  game %d: P became empty at ply %ld after the cap\n", games, plies);
            skip = 1;  /* nothing left to track; resume at the next game */
        }
    }
    if (have) { overflow_games += game_overflowed; belief_free(&b); }
    printf("games %d, plies %ld: true position missing %ld times while exact (%ld after the cap); |P| mean %.0f max %zu; update mean %.4f s max %.3f s; games that hit the cap %ld (cap %zu)\n",
           games, plies, unsound, lost_after_cap, sum_p / (plies ? plies : 1), max_p, sum_t / (plies ? plies : 1), max_t, overflow_games, cap);
    return unsound ? 1 : 0;
}
