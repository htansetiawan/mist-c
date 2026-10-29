/* Exports move-model training data: for every position where the mover's
 * moves are used, the features of each legal move and which one was played.
 * Input: tools/export_moves.py output. Output (binary, little-endian):
 * int32 F; then per position: int32 n, int32 played, int32 mover_id, float32 x[n][F],
 * where F = MM_FEATURES + 3: the dense features, then the moved piece type and the
 * mirrored from and to squares (movemodel.h) for the piece-square tables.
 * Mover names are listed on stderr as "mover <id> <name>".
 *
 *   build/move_features arena-train.txt out.bin [full=1]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/movemodel.h"

static int parse_uci(const Position *p, const char *uci, Move *out) {
    MoveList l; generate_moves(p, &l);
    for (int i = 0; i < l.count; ++i) { char u[6]; move_to_uci(l.moves[i], u); if (strcmp(u, uci) == 0) { *out = l.moves[i]; return i; } }
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: move_features IN OUT [full]\n"); return 2; }
    const int full = argc > 3 ? atoi(argv[3]) : 1;
    fow_init();
    FILE *in = fopen(argv[1], "r"), *out = fopen(argv[2], "wb");
    if (!in || !out) { perror("open"); return 2; }
    /* dense features, then the moved piece type and its mirrored from and to squares (as floats) */
    const int F = MM_FEATURES + 3;
    fwrite(&F, 4, 1, out);
    char names[64][64]; int n_names = 0;
    static char line[1 << 16];
    char white[64] = "-", black[64] = "-";
    long positions = 0, games = 0, bad = 0;
    float *x = malloc(sizeof(float) * 256 * F);
    while (fgets(line, sizeof line, in)) {
        if (strncmp(line, "game ", 5) == 0) { char id[64]; sscanf(line + 5, "%63s %63s %63s", id, white, black); continue; }
        int ids[2];
        for (int c = 0; c < 2; ++c) {
            const char *name = c == 0 ? white : black;
            ids[c] = -1;
            if (strcmp(name, "-") == 0) continue;
            int k = 0; while (k < n_names && strcmp(names[k], name) != 0) ++k;
            if (k == n_names && n_names < 64) { strcpy(names[n_names++], name); fprintf(stderr, "mover %d %s\n", k, name); }
            ids[c] = k < 64 ? k : -1;
        }
        Position p; pos_start(&p);
        Move previous = 0;
        ++games;
        for (char *tok = strtok(line, " \n"); tok; tok = strtok(NULL, " \n")) {
            Move m;
            const int played = parse_uci(&p, tok, &m);
            if (played < 0) { ++bad; break; }
            if (ids[p.side] >= 0) {
                MoveList l; generate_moves(&p, &l);
                const bb_t vis = full ? visible_squares(&p, p.side) : 0;
                for (int i = 0; i < l.count; ++i) {
                    float *xi = x + (size_t)i * F;
                    move_features_obs(&p, l.moves[i], previous, full, vis, xi);
                    xi[MM_FEATURES] = (float)p.piece[MOVE_FROM(l.moves[i])];
                    xi[MM_FEATURES + 1] = (float)move_mirror_square(p.side, MOVE_FROM(l.moves[i]));
                    xi[MM_FEATURES + 2] = (float)move_mirror_square(p.side, MOVE_TO(l.moves[i]));
                }
                const int head[3] = {l.count, played, ids[p.side]};
                fwrite(head, 4, 3, out);
                fwrite(x, sizeof(float), (size_t)l.count * F, out);
                ++positions;
            }
            Undo u; make_move(&p, m, &u);
            previous = m;
            if (king_square(&p, WHITE) < 0 || king_square(&p, BLACK) < 0) break;
        }
    }
    fclose(out);
    fprintf(stderr, "%ld games, %ld positions, %ld unparsable moves\n", games, positions, bad);
    free(x);
    return 0;
}
