/* mist-c: front-end bookkeeping. See front.h. */
#include "front.h"

#include <string.h>

#include "eval.h"

#define BIT_AT(sq) ((bb_t)1 << (sq))

static int piece_index(char c) {
    if (!c) return -1;
    const char *at = strchr("pnbrqk", c >= 'A' && c <= 'Z' ? c + 32 : c);
    return at ? (int)(at - "pnbrqk") : -1;
}

static int is_ours(char c, int us) { return piece_index(c) >= 0 && (us == WHITE) == (c >= 'A' && c <= 'Z'); }

void front_init(Front *f, int us) {
    memset(f, 0, sizeof(*f));
    f->us = us;
    static const int start[6] = {8, 2, 2, 2, 1, 1};
    memcpy(f->their, start, sizeof f->their);
    const int home = us == WHITE ? 0 : 56;
    f->castle = BIT_AT(home) | BIT_AT(home + 7);
}

void front_view_position(const char view[65], int us, bb_t castle, Position *p) {
    char board[65];
    for (int i = 0; i < 64; ++i) board[i] = view[i] == '?' ? '-' : view[i];
    board[64] = '\0';
    pos_from_board_string(p, board, us, castle, -1);
}

void front_see(Front *f, const char view[65]) {
    const int home = f->us == WHITE ? 0 : 56;
    const char rook = f->us == WHITE ? 'R' : 'r', king = f->us == WHITE ? 'K' : 'k';
    if (view[home + 4] != king) f->castle = 0;
    if (view[home] != rook) f->castle &= ~BIT_AT(home);
    if (view[home + 7] != rook) f->castle &= ~BIT_AT(home + 7);
}

static uint64_t own_after(const Front *f, const char view[65], Move m) {
    Position p; front_view_position(view, f->us, f->castle, &p);
    if (m) { Undo u; make_move(&p, m, &u); }
    char mine[65];
    for (int sq = 0; sq < 64; ++sq) mine[sq] = p.color[sq] == f->us ? "PNBRQK"[(int)p.piece[sq]] : '-';
    mine[64] = '\0';
    return view_hash(mine);
}

void front_play(Front *f, const char view[65], Move m) {
    const int from = MOVE_FROM(m), to = MOVE_TO(m);
    const int mover = piece_index(view[from]), taken = piece_index(view[to]);
    if (taken >= 0 && !is_ours(view[to], f->us)) { if (f->their[taken] > 0) --f->their[taken]; }
    else if (mover == PAWN && view[to] == '-' && (to - from) % 8 != 0 && f->their[PAWN] > 0) --f->their[PAWN];
    const uint64_t h = own_after(f, view, m);
    int c = 0;
    while (c < f->configs && f->config[c] != h) ++c;
    if (c == f->configs && f->configs < FRONT_MAX_CONFIGS) { f->config[c] = h; f->config_seen[c] = 0; ++f->configs; }
    if (c < f->configs) ++f->config_seen[c];
    if (mover == KING) f->castle = 0;
    f->castle &= ~(BIT_AT(from) | BIT_AT(to));
}

int front_lead(const Front *f, const char view[65]) {
    static const int value[6] = {1, 3, 3, 5, 9, 0};
    int ours = 0, theirs = 0;
    for (int sq = 0; sq < 64; ++sq) if (is_ours(view[sq], f->us)) ours += value[piece_index(view[sq])];
    for (int t = 0; t < 6; ++t) theirs += value[t] * f->their[t];
    return ours - theirs;
}

int front_repeats(const Front *f, const char view[65], const MoveList *moves, Move *out) {
    int n = 0;
    for (int i = 0; i < moves->count; ++i) {
        const uint64_t h = own_after(f, view, moves->moves[i]);
        for (int c = 0; c < f->configs; ++c)
            if (f->config[c] == h && f->config_seen[c] >= 2) { out[n++] = moves->moves[i]; break; }
    }
    return n;
}

Move front_best_allowed(const Front *f, const char view[65], const MoveList *allowed) {
    Position seen; front_view_position(view, f->us, f->castle, &seen);
    Move best = allowed->moves[0]; int best_cp = -1000000;
    for (int i = 0; i < allowed->count; ++i) {
        Position q = seen; Undo u; make_move(&q, allowed->moves[i], &u);
        const int cp = king_square(&q, f->us ^ 1) < 0 ? 100000 : evaluate_cp(&q, f->us);
        if (cp > best_cp) { best_cp = cp; best = allowed->moves[i]; }
    }
    return best;
}
