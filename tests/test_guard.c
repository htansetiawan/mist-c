/* The two ways mist-c v1 lost games to illegal moves, as tests for both
 * colours. (1) The moves view_moves allows are legal on the true board at
 * every decision of the recorded games, and it misses none but en passant.
 * (2) When the belief is rebuilt from the view, every rebuilt position shows
 * exactly our view and allows only moves legal on the true board. (3) The two
 * arena positions where v1, its belief empty, pushed a pawn into an unseen
 * piece (as Black: d2d1q, g6g5), and their mirror images for White. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/belief.h"

static int contains(const MoveList *l, Move m) {
    for (int i = 0; i < l->count; ++i) if (l->moves[i] == m) return 1;
    return 0;
}

static int material(const Position *p, int color, int their[6]) {
    int n = 0;
    for (int t = 0; t < 6; ++t) n += their[t] = __builtin_popcountll(p->by_type[t] & p->by_color[color]);
    return n;
}

static bb_t home_rooks(int us) { return us == WHITE ? 0x81ULL : 0x8100000000000000ULL; }

/* The view and move seen from the other side of the board. */
static void mirror(const char *view, char out[65]) {
    for (int sq = 0; sq < 64; ++sq) {
        const char c = view[sq ^ 56];
        out[sq] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    out[64] = '\0';
}

static int arena_case(const char *name, const char *view, int us, const char *bad) {
    MoveList l;
    view_moves(view, us, 0, &l);
    int allowed = 0;
    for (int i = 0; i < l.count; ++i) { char u[6]; move_to_uci(l.moves[i], u); if (strcmp(u, bad) == 0) allowed = 1; }
    Belief b = {0};
    b.us = us; b.cap = 1000; b.rng = 7;
    const int their[6] = {8, 2, 2, 2, 1, 1};
    const size_t n = belief_resample(&b, view, their, 0, 64, 20000);
    int rebuilt_bad = 0;
    for (size_t i = 0; i < b.count; ++i) {
        MoveList m; generate_moves(&b.items[i], &m);
        for (int j = 0; j < m.count; ++j) { char u[6]; move_to_uci(m.moves[j], u); if (strcmp(u, bad) == 0) rebuilt_bad = 1; }
    }
    printf("%s (%s): %d view moves, %s %s; rebuilt %zu positions, %s\n", name, us == WHITE ? "White" : "Black", l.count, bad,
           allowed ? "ALLOWED" : "excluded", n, rebuilt_bad ? "one ALLOWS it" : "none allows it");
    belief_free(&b);
    return allowed || rebuilt_bad || n == 0;
}

int main(int argc, char **argv) {
    fow_init(); zobrist_init();
    int failures = 0;
    /* (3) the arena positions, as Black and mirrored for White */
    /* views from the arena logs, with 'x' for '?': a literal "??-" is a C
     * trigraph for '~' */
    struct { const char *name, *view, *bad, *bad_white; } cases[] = {
        {"vs TA-P, ply 45", "xxxxxxxx-Pxpxx" "Pxx---x-xx-x---xx-p--q---pxp----p-x-p-x-bx--kr---N", "d2d1q", "d7d8q"},
        {"vs v25, ply 75", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxPxxxx-xx-x-xx-xx-p-xxpxx--p-----r-k", "g6g5", "g3g4"},
    };
    for (int i = 0; i < 2; ++i) {
        char view[65], flipped[65];
        for (int sq = 0; sq < 64; ++sq) view[sq] = cases[i].view[sq] == 'x' ? '?' : cases[i].view[sq];
        view[64] = '\0';
        if (strlen(cases[i].view) != 64) { printf("FAIL: case %d is not 64 squares\n", i); ++failures; continue; }
        failures += arena_case(cases[i].name, view, BLACK, cases[i].bad);
        mirror(view, flipped);
        failures += arena_case(cases[i].name, flipped, WHITE, cases[i].bad_white);
    }
    /* (1) and (2) on the recorded games */
    const char *path = argc > 1 ? argv[1] : "tests/traces.txt";
    const int every = argc > 2 ? atoi(argv[2]) : 3;
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 2; }
    char line[512];
    int us = WHITE, k = 0;
    long decisions[2] = {0}, unsound[2] = {0}, missed[2] = {0}, missed_ep[2] = {0};
    long rebuilt[2] = {0}, rebuilt_failed[2] = {0}, rebuilt_unsound[2] = {0}, rebuilt_view[2] = {0}, rebuilt_truth[2] = {0};
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "game ", 5) == 0) { char id[64], side[4]; sscanf(line + 5, "%63s %3s", id, side); us = side[0] == 'w' ? WHITE : BLACK; continue; }
        char who[8], uci[8], view[65], board[65]; int side, ep; uint64_t castle;
        if (sscanf(line, "%7s %7s %64s %64s %d %" SCNu64 " %d", who, uci, view, board, &side, &castle, &ep) != 7) continue;
        if (strcmp(who, "them") != 0 || side != us) continue;   /* our turn after their move */
        Position truth; pos_from_board_string(&truth, board, side, castle, ep);
        MoveList legal, safe;
        generate_moves(&truth, &legal);
        const bb_t our_castle = castle & home_rooks(us);
        view_moves(view, us, our_castle, &safe);
        ++decisions[us];
        for (int i = 0; i < safe.count; ++i)
            if (!contains(&legal, safe.moves[i])) {
                if (unsound[us]++ < 5) { char u[6]; move_to_uci(safe.moves[i], u); printf("UNSOUND: view_moves allows %s, illegal on the true board\n", u); }
            }
        for (int i = 0; i < legal.count; ++i)
            if (!contains(&safe, legal.moves[i])) {
                const Move m = legal.moves[i];
                if (truth.piece[MOVE_FROM(m)] == PAWN && MOVE_TO(m) == truth.ep) {
                    ++missed_ep[us];
                    if (getenv("GUARD_VERBOSE")) { char u[6]; move_to_uci(m, u); printf("en passant %s not proven by the view %s\n", u, view); }
                }
                else if (missed[us]++ < 5) { char u[6]; move_to_uci(m, u); printf("MISSED: %s legal but not allowed\n", u); }
            }
        if (k++ % every) continue;
        /* (2) rebuild from the view with the true material */
        int their[6]; material(&truth, us ^ 1, their);
        Belief b = {0};
        b.us = us; b.cap = 1000; b.rng = 99 + (uint64_t)k;
        const size_t n = belief_resample(&b, view, their, our_castle, 64, 4000);
        ++rebuilt[us];
        if (n == 0) { ++rebuilt_failed[us]; continue; }
        int view_bad = 0, move_bad = 0;
        for (size_t i = 0; i < b.count; ++i) {
            char seen[65]; observation(&b.items[i], us, seen);
            if (memcmp(seen, view, 64) != 0) view_bad = 1;
            MoveList m; generate_moves(&b.items[i], &m);
            for (int j = 0; j < m.count; ++j) if (!contains(&legal, m.moves[j])) move_bad = 1;
        }
        rebuilt_view[us] += view_bad; rebuilt_unsound[us] += move_bad;
        rebuilt_truth[us] += belief_contains(&b, &truth);
        belief_free(&b);
    }
    fclose(f);
    for (int c = 0; c < 2; ++c) {
        printf("%s: %ld decisions; view_moves illegal %ld, missed %ld (+%ld en passant); rebuilt %ld times: none fit %ld, view mismatch %ld, illegal moves %ld, true board among them %ld\n",
               c == WHITE ? "as White" : "as Black", decisions[c], unsound[c], missed[c], missed_ep[c],
               rebuilt[c], rebuilt_failed[c], rebuilt_view[c], rebuilt_unsound[c], rebuilt_truth[c]);
        failures += unsound[c] > 0 || missed[c] > 0 || rebuilt_view[c] > 0 || rebuilt_unsound[c] > 0 || rebuilt_failed[c] > 0;
        if (decisions[c] == 0) { printf("FAIL: no decisions as %s in the traces\n", c == WHITE ? "White" : "Black"); ++failures; }
    }
    printf("%s\n", failures ? "test_guard: FAILED" : "test_guard: passed");
    return failures ? 1 : 0;
}
