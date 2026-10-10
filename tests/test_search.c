/* M3: the two-ply belief subgame. (1) With the enemy king visible and
 * capturable, the search takes it. (2) On recorded arena games, with the exact
 * belief tracked as the game went, every chosen move is legal on the true
 * board; we report how often it would have left our king capturable there
 * while a safe move existed, and the time per decision. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/search.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static int legal_in(const Position *p, Move m) {
    MoveList l; generate_moves(p, &l);
    for (int i = 0; i < l.count; ++i) if (l.moves[i] == m) return 1;
    return 0;
}

int main(int argc, char **argv) {
    fow_init(); zobrist_init();
    int failures = 0;
    /* (1) White rook h1, open h-file, black king h8: Rxh8 captures the king. */
    {
        /* built square by square: a hand-typed 63-character string once put the
         * king on g8 and made this test check the wrong thing */
        char board[65];
        memset(board, '-', 64); board[64] = '\0';
        board[4] = 'K'; board[7] = 'R'; board[54] = 'p'; board[63] = 'k';   /* Ke1 Rh1, Black pawn g7, king h8 */
        Position p;
        if (pos_from_board_string(&p, board, WHITE, 0, -1) != 0) { puts("FAIL: test board did not parse"); ++failures; }
        Belief b = {0}; b.items = &p; b.count = 1; b.us = WHITE;
        SearchParams sp = {16, 50, 1}; SearchResult r;
        char uci[6];
        if (!search_decide(&b, &sp, &r)) { puts("FAIL: no move"); ++failures; }
        move_to_uci(r.move, uci);
        printf("king capture test: chose %s (value %+.3f)\n", uci, r.value);
        if (strcmp(uci, "h1h8") != 0) { puts("FAIL: did not take the king"); ++failures; }
    }
    /* (2) recorded games */
    const char *path = argc > 1 ? argv[1] : "tests/traces.txt";
    const int max_games = argc > 2 ? atoi(argv[2]) : 3, max_decisions = argc > 3 ? atoi(argv[3]) : 25;
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 2; }
    char line[512], prev_board[65] = "RNBQKBNRPPPPPPPP--------------------------------pppppppprnbqkbnr";
    int prev_side = WHITE, prev_ep = -1, games = 0, decisions = 0, illegal = 0, unsafe = 0, unsafe_avoidable = 0;
    unsigned long long prev_castle = 0x8100000000000081ULL;
    double total_t = 0, max_t = 0;
    Belief b = {0}; int have = 0, per_game = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "game ", 5) == 0) {
            if (have) belief_free(&b);
            if (games >= max_games) { have = 0; break; }
            char id[64], side[4]; sscanf(line + 5, "%63s %3s", id, side);
            belief_init_start(&b, side[0] == 'w' ? WHITE : BLACK, 100000);
            have = 1; ++games; per_game = 0;
            strcpy(prev_board, "RNBQKBNRPPPPPPPP--------------------------------pppppppprnbqkbnr");
            prev_side = WHITE; prev_ep = -1; prev_castle = 0x8100000000000081ULL;
            continue;
        }
        char who[8], uci[8], view[65], board[65]; int side, ep; unsigned long long castle;
        if (sscanf(line, "%7s %7s %64s %64s %d %llu %d", who, uci, view, board, &side, &castle, &ep) != 7) continue;
        if (strcmp(who, "us") == 0 && b.count > 0 && !b.overflowed && per_game < max_decisions) {
            Position truth; pos_from_board_string(&truth, prev_board, prev_side, prev_castle, prev_ep);
            SearchParams sp = {64, 60, (unsigned long long)decisions + 1}; SearchResult r;
            const double t0 = now();
            const int ok = search_decide(&b, &sp, &r);
            const double dt = now() - t0;
            total_t += dt; if (dt > max_t) max_t = dt;
            ++decisions; ++per_game;
            if (!ok || !legal_in(&truth, r.move)) { ++illegal; char u[6]; move_to_uci(r.move, u); printf("FAIL: illegal %s\n", u); }
            else {
                Position t = truth; Undo u; make_move(&t, r.move, &u);
                if (mover_left_king_capturable(&t)) {
                    ++unsafe;
                    MoveList l; generate_moves(&truth, &l); int safe = 0;
                    for (int i = 0; i < l.count && !safe; ++i) { Position q = truth; Undo w; make_move(&q, l.moves[i], &w); safe = !mover_left_king_capturable(&q); }
                    unsafe_avoidable += safe;
                }
            }
        }
        /* advance the belief exactly as test_belief does */
        if (strcmp(who, "us") == 0) {
            Move m = 0; MoveList l; generate_moves(&b.items[0], &l);
            for (int i = 0; i < l.count; ++i) { char u[6]; move_to_uci(l.moves[i], u); if (strcmp(u, uci) == 0) { m = l.moves[i]; break; } }
            belief_after_own(&b, m, view);
        } else {
            belief_after_opponent(&b, view);
        }
        memcpy(prev_board, board, 65); prev_side = side; prev_castle = castle; prev_ep = ep;
    }
    if (have) belief_free(&b);
    fclose(f);
    printf("%d games, %d decisions: illegal %d; left our king capturable on the true board %d (a safe move existed in %d); "
           "time per decision mean %.3f s, max %.3f s\n", games, decisions, illegal, unsafe, unsafe_avoidable,
           decisions ? total_t / decisions : 0.0, max_t);
    if (illegal) ++failures;
    printf("%s\n", failures ? "test_search: FAILED" : "test_search: passed");
    return failures ? 1 : 0;
}
