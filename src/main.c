/* mist-c: referee-protocol front end (milestone M5, first version).
 *
 * The course referee sends "fow", then "<w|b> <base> <increment>"; the bot
 * answers "ready". Then a 64-character view arrives before every ply, and our
 * remaining seconds when it is our turn; we answer "move <uci>". The exact
 * belief is updated from each view: the first view is the start (nothing
 * moved), a view following our own move is updated with that move, any other
 * view by every opponent move consistent with it. If the capped belief ever
 * loses every position, the bot falls back to the position made of what it
 * sees (our pieces and the visible enemy pieces) so it always plays a legal
 * move.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "search.h"

static int is_view(const char *s) {
    if (strlen(s) != 64) return 0;
    for (int i = 0; i < 64; ++i) if (!strchr("PNBRQKpnbrqk-?", s[i])) return 0;
    return 1;
}

/* What we see, as a position: our pieces and the visible enemy pieces. */
static void view_position(const char *view, int us, Position *p, bb_t castle) {
    char board[65];
    for (int i = 0; i < 64; ++i) board[i] = view[i] == '?' ? '-' : view[i];
    board[64] = '\0';
    pos_from_board_string(p, board, us, castle, -1);
}

int main(void) {
    char line[256];
    if (!fgets(line, sizeof line, stdin) || strncmp(line, "fow", 3) != 0) return 0;
    char side[4]; double base = 180, increment = 2;
    if (!fgets(line, sizeof line, stdin) || sscanf(line, "%3s %lf %lf", side, &base, &increment) < 1) return 0;
    const int us = side[0] == 'w' ? WHITE : BLACK;
    fow_init(); zobrist_init();
    const size_t cap = getenv("MIST_CAP") ? (size_t)strtoull(getenv("MIST_CAP"), NULL, 10) : 200000;
    Belief b; belief_init_start(&b, us, cap);
    printf("ready\n"); fflush(stdout);

    char view[65] = "";
    int views = 0, moved = 0, decisions = 0;
    Move last = 0;
    unsigned long long seed = getenv("FOW_SEED") ? strtoull(getenv("FOW_SEED"), NULL, 10) : 1;
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (is_view(line)) {
            memcpy(view, line, 65);
            if (views++ > 0 && b.count > 0) {
                if (moved) belief_after_own(&b, last, view);
                else belief_after_opponent(&b, view);
            }
            moved = 0;
            continue;
        }
        char *end; const double remaining = strtod(line, &end);
        if (end == line) continue;
        /* our turn: spend about remaining/30 + increment, as samples and iterations */
        SearchParams sp = {128, 100, seed + (unsigned long long)decisions};
        if (remaining < 20) { sp.samples = 32; sp.iterations = 30; }
        SearchResult r; int ok = 0;
        if (b.count > 0) ok = search_decide(&b, &sp, &r);
        if (!ok) {
            /* fallback: decide on the position we can see */
            Position seen; view_position(view, us, &seen, b.count ? b.items[0].castle_rooks : 0);
            Belief one = {0}; one.items = &seen; one.count = 1; one.us = us;
            ok = search_decide(&one, &sp, &r);
            fprintf(stderr, "mist: belief empty, decided on the visible position\n");
        }
        if (!ok) { fprintf(stderr, "mist: no legal move\n"); break; }
        char uci[6]; move_to_uci(r.move, uci);
        fprintf(stderr, "mist: n=%d |P|=%zu%s samples=%d moves=%d infosets=%d nodes=%d p=%.2f v=%+.3f clock=%.1f move=%s\n",
                ++decisions, b.count, b.overflowed ? "(capped)" : "", r.samples, r.our_moves, r.opponent_infosets, r.nodes,
                r.probability, r.value, remaining, uci);
        printf("move %s\n", uci); fflush(stdout);
        last = r.move; moved = 1;
    }
    belief_free(&b);
    return 0;
}
