/* Builds the empirical opponent prior: how often each move followed each
 * information history in the arena. A history is the mover's own views and
 * moves so far, hashed exactly as belief.c keeps it for every world
 * (history_mix over view_hash of the start view, then per ply the mover's move
 * and its view after it, or just its view after the other side's move), so the
 * engine can look a world's b->hist[i] up directly.
 * Input: tools/export_moves.py output (named sides only; "-" sides and our own
 * moves are skipped). Output: little-endian records sorted by (hash, move):
 *   uint64 hash, uint32 move, uint32 count
 * after a 16-byte header "MISTHIST" + uint32 version 1 + uint32 record count.
 * Histories seen fewer than MIN_GAMES times are dropped.
 *
 *   build/history_table export.txt table.bin [min_games=2] [skip_name=IAmAVirus]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/belief.h"

typedef struct { uint64_t hash; uint32_t move, count; } Rec;

static int parse_uci(const Position *p, const char *uci, Move *out) {
    MoveList l; generate_moves(p, &l);
    for (int i = 0; i < l.count; ++i) { char u[6]; move_to_uci(l.moves[i], u); if (strcmp(u, uci) == 0) { *out = l.moves[i]; return 1; } }
    return 0;
}

static int by_hash_move(const void *a, const void *b) {
    const Rec *x = a, *y = b;
    if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
    if (x->move != y->move) return x->move < y->move ? -1 : 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: history_table IN OUT [min_games] [skip_name]\n"); return 2; }
    const unsigned min_games = argc > 3 ? (unsigned)atoi(argv[3]) : 2;
    const char *skip = argc > 4 ? argv[4] : "IAmAVirus";
    fow_init();
    FILE *in = fopen(argv[1], "r");
    if (!in) { perror("open"); return 2; }
    static char line[1 << 16];
    char white[64] = "-", black[64] = "-";
    size_t n = 0, cap = 1 << 20;
    Rec *recs = malloc(cap * sizeof(Rec));
    long games = 0, bad = 0;
    while (fgets(line, sizeof line, in)) {
        if (strncmp(line, "game ", 5) == 0) { char id[64]; sscanf(line + 5, "%63s %63s %63s", id, white, black); continue; }
        const int use[2] = {strcmp(white, "-") != 0 && strcmp(white, skip) != 0, strcmp(black, "-") != 0 && strcmp(black, skip) != 0};
        if (!use[0] && !use[1]) continue;
        ++games;
        Position p; pos_start(&p);
        char view[65];
        uint64_t h[2];
        for (int s = 0; s < 2; ++s) { observation(&p, s, view); h[s] = history_mix(0, view_hash(view)); }
        for (char *tok = strtok(line, " \n"); tok; tok = strtok(NULL, " \n")) {
            Move m;
            if (!parse_uci(&p, tok, &m)) { ++bad; break; }
            const int mover = p.side;
            if (use[mover]) {
                if (n == cap) { cap *= 2; recs = realloc(recs, cap * sizeof(Rec)); }
                recs[n].hash = h[mover]; recs[n].move = (uint32_t)m; recs[n].count = 1; ++n;
            }
            Undo u; make_move(&p, m, &u);
            if (king_square(&p, WHITE) < 0 || king_square(&p, BLACK) < 0) break;
            for (int s = 0; s < 2; ++s) {
                observation(&p, s, view);
                h[s] = history_mix(s == mover ? history_mix(h[s], m) : h[s], view_hash(view));
            }
        }
    }
    fclose(in);
    qsort(recs, n, sizeof(Rec), by_hash_move);
    /* merge equal (hash, move); then drop histories below min_games */
    size_t w = 0;
    for (size_t i = 0; i < n; ++i) {
        if (w > 0 && recs[w - 1].hash == recs[i].hash && recs[w - 1].move == recs[i].move) recs[w - 1].count += recs[i].count;
        else recs[w++] = recs[i];
    }
    n = w; w = 0;
    size_t kept_hist = 0, all_hist = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i; unsigned total = 0;
        while (j < n && recs[j].hash == recs[i].hash) total += recs[j++].count;
        ++all_hist;
        if (total >= min_games) { ++kept_hist; for (size_t k = i; k < j; ++k) recs[w++] = recs[k]; }
        i = j;
    }
    n = w;
    FILE *out = fopen(argv[2], "wb");
    if (!out) { perror("open out"); return 2; }
    const uint32_t version = 1, count = (uint32_t)n;
    fwrite("MISTHIST", 1, 8, out); fwrite(&version, 4, 1, out); fwrite(&count, 4, 1, out);
    fwrite(recs, sizeof(Rec), n, out);
    fclose(out);
    fprintf(stderr, "%ld games, %ld unparsable moves; %zu histories seen, %zu kept (>= %u games), %zu (history, move) records, %.1f MB\n",
            games, bad, all_hist, kept_hist, min_games, n, (16.0 + 16.0 * n) / 1048576.0);
    return 0;
}
