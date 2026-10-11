/* Stockfish leaf evaluator, both colours: the FEN; a position and its mirror
 * (colours swapped) score every move alike; a Fog of War move that is not legal
 * chess (castling through an attacked square) gets no score; a missing binary
 * falls back. Needs MIST_STOCKFISH=path (skipped otherwise). */
#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/sfeval.h"

static int mirror_sq(int sq) { return sq ^ 56; }

static void mirror(const Position *p, Position *q) {
    char text[65], m[65];
    pos_to_board_string(p, text);
    for (int sq = 0; sq < 64; ++sq) {
        const char c = text[sq];
        m[mirror_sq(sq)] = isupper((unsigned char)c) ? (char)tolower((unsigned char)c) : islower((unsigned char)c) ? (char)toupper((unsigned char)c) : c;
    }
    m[64] = '\0';
    bb_t rooks = 0;
    for (int sq = 0; sq < 64; ++sq) if (p->castle_rooks & ((bb_t)1 << sq)) rooks |= (bb_t)1 << mirror_sq(sq);
    pos_from_board_string(q, m, p->side ^ 1, rooks, p->ep >= 0 ? mirror_sq(p->ep) : -1);
}

static void check_mirror(const char *name, const Position *p) {
    Position q; mirror(p, &q);
    MoveList a, b; generate_moves(p, &a); generate_moves(&q, &b);
    int cpa[256], fa[256], cpb[256], fb[256];
    const int na = sf_children(p, &a, cpa, fa), nb = sf_children(&q, &b, cpb, fb);
    if (!(na > 0 && na == nb)) { char f1[100], f2[100]; sf_fen(p, f1); sf_fen(&q, f2); printf("MISMATCH %s: %d of %d | %s: %d of %d\n", f1, na, a.count, f2, nb, b.count); }
    assert(na > 0 && na == nb);
    int compared = 0;
    double worst = 0;
    for (int i = 0; i < a.count; ++i) {
        const Move m = a.moves[i];
        const Move mm = MAKE_MOVE(mirror_sq(MOVE_FROM(m)), mirror_sq(MOVE_TO(m)), MOVE_PROMO(m));
        for (int j = 0; j < b.count; ++j) {
            if (b.moves[j] != mm) continue;
            assert(fa[i] == fb[j]);
            if (fa[i]) {
                const double d = fabs(tanh(cpa[i] / 400.0) - tanh(cpb[j] / 400.0));
                if (d > worst) worst = d;
                ++compared;
            }
        }
    }
    char fen[100]; sf_fen(p, fen);
    /* Stockfish 19 at depth 1 is not exactly colour-symmetric (even from a
     * fresh hash: f7f6 scores -878 here and -730 mirrored), so compare the
     * values the solver sees, tanh(cp/400); a colour bug flips their sign */
    printf("%-28s %s: %d moves scored, %d compared with the mirror (colours swapped): largest value difference %.3f\n", name, fen, na, compared, worst);
    assert(worst <= 0.12);
}

static void board(char out[65], const char *pieces) {   /* "Ke1 Ra1 ... kg8" */
    memset(out, '-', 64); out[64] = '\0';
    for (const char *s = pieces; *s; ) {
        while (*s == ' ') ++s;
        if (!*s) break;
        out[(s[2] - '1') * 8 + (s[1] - 'a')] = s[0];
        s += 3;
    }
}

static void play(Position *p, const char *moves) {
    char u[8]; int n = 0;
    while (sscanf(moves, "%7s%n", u, &n) == 1) {
        MoveList l; generate_moves(p, &l);
        int found = 0;
        for (int i = 0; i < l.count && !found; ++i) {
            char v[6]; move_to_uci(l.moves[i], v);
            if (strcmp(u, v) == 0) { Undo undo; make_move(p, l.moves[i], &undo); found = 1; }
        }
        assert(found);
        moves += n;
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fow_init();
    Position p;
    pos_start(&p);
    char fen[100];
    sf_fen(&p, fen);
    assert(strcmp(fen, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") == 0);
    if (!getenv("MIST_STOCKFISH")) { printf("test_sf: MIST_STOCKFISH not set, Stockfish checks skipped\n"); return 0; }
    assert(!sf_start("/nonexistent/stockfish") && !sf_ready());   /* falls back */
    assert(sf_start(getenv("MIST_STOCKFISH")) && sf_ready());
    check_mirror("start, White to move", &p);
    /* a middlegame reached by play, Black to move, with the e.p. square set */
    pos_start(&p);
    play(&p, "e2e4 e7e6 d2d4 d7d5 b1c3 g8f6 c1g5 f8e7 e4e5 f6d7 g5e7 d8e7 f2f4 c7c5 g1f3 b8c6 d1d2 a7a6 d4c5 e7c5 a2a4 b7b5");
    check_mirror("middlegame after 22 plies", &p);
    play(&p, "a4b5");
    check_mirror("middlegame, Black to move", &p);
    /* castling through an attacked square: legal in Fog of War, not in chess
     * (the bishop on b5 sees f1) */
    char b[65];
    board(b, "Ke1 Ra1 Rh1 bb5 kg8");
    assert(pos_from_board_string(&p, b, WHITE, ((bb_t)1 << 0) | ((bb_t)1 << 7), -1) == 0);
    MoveList l; generate_moves(&p, &l);
    int cp[256], found[256];
    assert(sf_children(&p, &l, cp, found) > 0);
    int castle_scored = -1;
    for (int i = 0; i < l.count; ++i) {
        char u[6]; move_to_uci(l.moves[i], u);
        if (strcmp(u, "e1g1") == 0) castle_scored = found[i];
    }
    printf("castling through f1 (attacked by the bishop on b5): %s by Stockfish\n", castle_scored == 0 ? "not scored" : castle_scored == 1 ? "scored" : "not generated");
    assert(castle_scored == 0);
    /* a position Stockfish refuses (the side to move can take the king) is
     * never sent, and the engine keeps answering */
    board(b, "Ke1 Ra1 Rh1 ka8");
    assert(pos_from_board_string(&p, b, WHITE, ((bb_t)1 << 0) | ((bb_t)1 << 7), -1) == 0);
    generate_moves(&p, &l);
    assert(sf_children(&p, &l, cp, found) == 0 && sf_ready());
    pos_start(&p); generate_moves(&p, &l);
    assert(sf_children(&p, &l, cp, found) == l.count);
    printf("a position where the king can be taken: not sent; Stockfish still answers\n");
    long calls; double seconds; sf_stats(&calls, &seconds);
    printf("test_sf: passed (%ld calls, %.2f ms each)\n", calls, 1000 * seconds / (double)calls);
    sf_stop();
    return 0;
}
