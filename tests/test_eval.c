/* Evaluator checks, both colours: the fog-line counts around each king, and
 * colour symmetry (a position and its mirror, colours swapped, score alike). */
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "../src/eval.h"

/* a1..h8 */
static void mirror(const char *in, char *out) {
    for (int sq = 0; sq < 64; ++sq) {
        const char c = in[sq];
        out[sq ^ 56] = isupper((unsigned char)c) ? (char)tolower((unsigned char)c) : islower((unsigned char)c) ? (char)toupper((unsigned char)c) : c;
    }
    out[64] = '\0';
}

static void board(char out[65], const char *pieces) {   /* "Kg1 Rf1 ... kg8" */
    memset(out, '-', 64); out[64] = '\0';
    for (const char *s = pieces; *s; ) {
        while (*s == ' ') ++s;
        if (!*s) break;
        const char t = s[0]; const int f = s[1] - 'a', r = s[2] - '1';
        out[r * 8 + f] = t;
        s += 3;
    }
}

static void check(const char *name, const char *text, int color, int lines, int knights) {
    Position p, q; char m[65]; int fog[2], fog_m[2];
    pos_from_board_string(&p, text, WHITE, 0, -1);
    mirror(text, m);
    pos_from_board_string(&q, m, BLACK, 0, -1);
    eval_fog(&p, color, fog);
    eval_fog(&q, color ^ 1, fog_m);
    printf("%-34s %s: lines %d knight squares %d | mirrored for %s: %d %d\n", name, color == WHITE ? "white" : "black",
           fog[0], fog[1], color == WHITE ? "black" : "white", fog_m[0], fog_m[1]);
    assert(fog[0] == lines && fog[1] == knights);
    assert(fog_m[0] == lines && fog_m[1] == knights);
    assert(evaluate_cp(&p, WHITE) == evaluate_cp(&q, BLACK));
    assert(evaluate_cp(&p, BLACK) == evaluate_cp(&q, WHITE));
}

int main(void) {
    fow_init();
    char b[65];
    /* castled behind three pawns and a rook: no line reaches the fog; e2 is the
     * one unseen knight square (f3 and h3 are seen by the pawn pushes) */
    board(b, "Kg1 Rf1 Pf2 Pg2 Ph2 Pa2 kg8 qd8 rf8 nb8 pf7 pg7 ph7");
    check("castled king", b, WHITE, 0, 1);
    /* the same without knights: no knight squares counted */
    board(b, "Kg1 Rf1 Pf2 Pg2 Ph2 Pa2 kg8 qd8 rf8 pf7 pg7 ph7");
    check("castled king, no enemy knights", b, WHITE, 0, 0);
    /* a bare king on e1: the e-file, both diagonals and both sides of the
     * first rank run into the fog */
    board(b, "Ke1 Pa2 Pb2 ke8 qd8 ra8 bc8 nb8");
    check("bare king in the centre", b, WHITE, 5, 4);
    /* only bishops left: the diagonals count, the file and rank do not */
    board(b, "Ke1 Pa2 Pb2 ke8 bc8");
    check("bare king against a bishop", b, WHITE, 2, 0);
    /* the black king, the same shapes */
    board(b, "Ke1 Qd1 Ra1 Bc1 Nb1 ke8 pa7 pb7");
    check("bare black king in the centre", b, BLACK, 5, 4);
    printf("test_eval: passed\n");
    return 0;
}
