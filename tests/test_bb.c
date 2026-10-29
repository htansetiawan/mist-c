/* Differential test of the bitboard rules (board.c) against the reference
 * implementation (tests/ref_board.c): on every position of the given files
 * (vectors as in test_rules, traces as in test_belief), every legal
 * successor of each, and each with the side to move flipped, both colours'
 * visibility, observation strings, attack tests on all 64 squares, attack
 * sets of every piece, the move list in order, and the king-capturable test
 * must agree exactly. Exits non-zero on any mismatch. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fow.h"

void ref_fow_init(void);
bb_t ref_attacks_from(const Position *p, int sq);
int ref_is_attacked(const Position *p, int sq, int by_color);
void ref_generate_moves(const Position *p, MoveList *l);
int ref_mover_left_king_capturable(const Position *p);
bb_t ref_visible_squares(const Position *p, int color);
void ref_observation(const Position *p, int color, char out[65]);

static long positions = 0, mismatches = 0;

static void report(const char *what, const Position *p, int color) {
    if (mismatches++ < 10) {
        char board[65];
        pos_to_board_string(p, board);
        fprintf(stderr, "MISMATCH %s: board %s side %d castle %" PRIu64 " ep %d colour %d\n", what, board, p->side, p->castle_rooks, p->ep, color);
    }
}

static void check_one(const Position *p) {
    ++positions;
    for (int color = 0; color < 2; ++color) {
        if (visible_squares(p, color) != ref_visible_squares(p, color)) report("visible_squares", p, color);
        char a[65], b[65];
        observation(p, color, a); ref_observation(p, color, b);
        if (strcmp(a, b) != 0) report("observation", p, color);
        for (int sq = 0; sq < 64; ++sq)
            if (is_attacked(p, sq, color) != ref_is_attacked(p, sq, color)) { report("is_attacked", p, color); break; }
    }
    for (int sq = 0; sq < 64; ++sq)
        if (attacks_from(p, sq) != ref_attacks_from(p, sq)) { report("attacks_from", p, sq); break; }
    MoveList l, r;
    generate_moves(p, &l); ref_generate_moves(p, &r);
    if (l.count != r.count || memcmp(l.moves, r.moves, (size_t)l.count * sizeof(Move)) != 0) report("generate_moves", p, p->side);
    if (mover_left_king_capturable(p) != ref_mover_left_king_capturable(p)) report("mover_left_king_capturable", p, p->side);
}

/* The position, its side-flipped twin, and every legal successor of both. */
static void check(const Position *p0) {
    for (int flip = 0; flip < 2; ++flip) {
        Position p = *p0;
        if (flip) p.side ^= 1;
        check_one(&p);
        MoveList l;
        ref_generate_moves(&p, &l);
        for (int i = 0; i < l.count; ++i) {
            Undo u;
            make_move(&p, l.moves[i], &u);
            check_one(&p);
            unmake_move(&p, l.moves[i], &u);
        }
    }
}

static void run_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(2); }
    static char line[1 << 16];
    long before = positions;
    while (fgets(line, sizeof line, f)) {
        char board[65], who[8], uci[8], view[65];
        int side, ep; uint64_t castle;
        Position p;
        char *bar = strchr(line, '|');
        if (bar) {
            /* vectors: board side castle ep | moves | views */
            *bar = '\0';
            if (sscanf(line, "%64s %d %" SCNu64 " %d", board, &side, &castle, &ep) != 4) continue;
        } else if (strncmp(line, "game ", 5) == 0) {
            continue;
        } else if (sscanf(line, "%7s %7s %64s %64s %d %" SCNu64 " %d", who, uci, view, board, &side, &castle, &ep) != 7) {
            continue;
        }
        if (pos_from_board_string(&p, board, side, castle, ep)) { fprintf(stderr, "bad board %s\n", board); exit(2); }
        check(&p);
    }
    fclose(f);
    printf("%s: %ld positions checked\n", path, positions - before);
}

int main(int argc, char **argv) {
    fow_init();
    ref_fow_init();
    if (argc < 2) { fprintf(stderr, "usage: test_bb FILE...\n"); return 2; }
    for (int i = 1; i < argc; ++i) run_file(argv[i]);
    printf("positions %ld (with side flips and successors): mismatches %ld\n", positions, mismatches);
    return mismatches ? 1 : 0;
}
