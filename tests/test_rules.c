/* M1 conformance: legal moves, both views, and make/unmake round trips, against
 * vectors produced by the referee-verified rules (tests/gen_vectors.py). */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fow.h"

static int cmp_str(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "tests/vectors.txt";
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 2; }
    fow_init();
    static char line[1 << 16];
    long positions = 0, move_errors = 0, view_errors = 0, roundtrip_errors = 0;
    while (fgets(line, sizeof line, f)) {
        char board[65], expected_moves[1 << 15], view_w[65], view_b[65];
        int side, ep;
        uint64_t castle;
        char *bar1 = strchr(line, '|'), *bar2 = bar1 ? strchr(bar1 + 1, '|') : NULL;
        if (!bar1 || !bar2) continue;
        *bar1 = '\0'; *bar2 = '\0';
        if (sscanf(line, "%64s %d %" SCNu64 " %d", board, &side, &castle, &ep) != 4) continue;
        strncpy(expected_moves, bar1 + 1, sizeof expected_moves - 1);
        expected_moves[sizeof expected_moves - 1] = '\0';
        if (sscanf(bar2 + 1, "%64s %64s", view_w, view_b) != 2) continue;
        Position p;
        if (pos_from_board_string(&p, board, side, castle, ep)) { fprintf(stderr, "bad board %s\n", board); return 2; }
        ++positions;
        MoveList l;
        generate_moves(&p, &l);
        char ucis[256][6];
        for (int i = 0; i < l.count; ++i) move_to_uci(l.moves[i], ucis[i]);
        qsort(ucis, (size_t)l.count, sizeof ucis[0], cmp_str);
        char got[1 << 15]; got[0] = '\0';
        for (int i = 0; i < l.count; ++i) { if (i) strcat(got, " "); strcat(got, ucis[i]); }
        char want[1 << 15]; int n = 0;
        for (char *s = expected_moves; *s; ++s) if (*s != '\n' && !(n == 0 && *s == ' ')) want[n++] = *s;
        while (n > 0 && want[n - 1] == ' ') --n;
        want[n] = '\0';
        if (strcmp(got, want) != 0) {
            if (move_errors++ < 5) fprintf(stderr, "moves differ at %s side %d ep %d\n  got  %s\n  want %s\n", board, side, ep, got, want);
        }
        char ow[65], ob[65];
        observation(&p, WHITE, ow); observation(&p, BLACK, ob);
        if (strcmp(ow, view_w) || strcmp(ob, view_b)) {
            if (view_errors++ < 5) fprintf(stderr, "views differ at %s side %d ep %d\n  got  %s %s\n  want %s %s\n", board, side, ep, ow, ob, view_w, view_b);
        }
        char before[65], after[65];
        pos_to_board_string(&p, before);
        for (int i = 0; i < l.count; ++i) {
            Undo u; Position copy = p;
            make_move(&p, l.moves[i], &u);
            unmake_move(&p, l.moves[i], &u);
            pos_to_board_string(&p, after);
            if (strcmp(before, after) || p.castle_rooks != copy.castle_rooks || p.ep != copy.ep || p.side != copy.side) {
                if (roundtrip_errors++ < 5) { char uci[6]; move_to_uci(l.moves[i], uci); fprintf(stderr, "make/unmake mismatch for %s at %s\n", uci, before); }
                p = copy;
            }
        }
    }
    fclose(f);
    printf("positions %ld: move-set mismatches %ld, view mismatches %ld, make/unmake mismatches %ld\n",
           positions, move_errors, view_errors, roundtrip_errors);
    return (move_errors || view_errors || roundtrip_errors) ? 1 : 0;
}
