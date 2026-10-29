/* Reference rules for tests/test_bb.c: the move-list visibility and the
 * piece-by-piece attack test as board.c had them before the bitboard tables
 * (mist-c 2803b68), under ref_ names. The differential test compares the
 * current board.c against these on every position of the traces and
 * vectors. Not linked into the engine. */
#include "../src/fow.h"

#include <string.h>

static bb_t knight_table[64], king_table[64];
static const int rook_dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
static const int bishop_dirs[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
static const char symbols[2][7] = {"PNBRQK", "pnbrqk"};

#define BIT(sq) ((bb_t)1 << (sq))

static int on_board(int file, int rank) { return file >= 0 && file < 8 && rank >= 0 && rank < 8; }

void ref_fow_init(void) {
    static const int kn[8][2] = {{1, 2}, {2, 1}, {-1, 2}, {-2, 1}, {1, -2}, {2, -1}, {-1, -2}, {-2, -1}};
    for (int sq = 0; sq < 64; ++sq) {
        int f = sq % 8, r = sq / 8;
        knight_table[sq] = king_table[sq] = 0;
        for (int i = 0; i < 8; ++i)
            if (on_board(f + kn[i][0], r + kn[i][1])) knight_table[sq] |= BIT((r + kn[i][1]) * 8 + f + kn[i][0]);
        for (int df = -1; df <= 1; ++df)
            for (int dr = -1; dr <= 1; ++dr)
                if ((df || dr) && on_board(f + df, r + dr)) king_table[sq] |= BIT((r + dr) * 8 + f + df);
    }
}

static bb_t slide(const Position *p, int sq, const int dirs[4][2]) {
    bb_t occupied = p->by_color[0] | p->by_color[1], out = 0;
    int f0 = sq % 8, r0 = sq / 8;
    for (int d = 0; d < 4; ++d) {
        for (int f = f0 + dirs[d][0], r = r0 + dirs[d][1]; on_board(f, r); f += dirs[d][0], r += dirs[d][1]) {
            out |= BIT(r * 8 + f);
            if (occupied & BIT(r * 8 + f)) break;
        }
    }
    return out;
}

bb_t ref_attacks_from(const Position *p, int sq) {
    int color = p->color[sq], f = sq % 8, r = sq / 8;
    switch (p->piece[sq]) {
        case PAWN: {
            int dr = color == WHITE ? 1 : -1;
            bb_t out = 0;
            if (on_board(f - 1, r + dr)) out |= BIT((r + dr) * 8 + f - 1);
            if (on_board(f + 1, r + dr)) out |= BIT((r + dr) * 8 + f + 1);
            return out;
        }
        case KNIGHT: return knight_table[sq];
        case BISHOP: return slide(p, sq, bishop_dirs);
        case ROOK: return slide(p, sq, rook_dirs);
        case QUEEN: return slide(p, sq, bishop_dirs) | slide(p, sq, rook_dirs);
        case KING: return king_table[sq];
        default: return 0;
    }
}

int ref_is_attacked(const Position *p, int sq, int by_color) {
    for (bb_t b = p->by_color[by_color]; b; b &= b - 1) {
        int from = __builtin_ctzll(b);
        if (ref_attacks_from(p, from) & BIT(sq)) return 1;
    }
    return 0;
}

static int ref_king_square(const Position *p, int color) {
    bb_t k = p->by_type[KING] & p->by_color[color];
    return k ? __builtin_ctzll(k) : -1;
}

static void add(MoveList *l, int from, int to, int promo) { l->moves[l->count++] = MAKE_MOVE(from, to, promo); }

static void add_pawn(MoveList *l, int from, int to, int color) {
    int rank = to / 8;
    if ((color == WHITE && rank == 7) || (color == BLACK && rank == 0)) {
        add(l, from, to, QUEEN); add(l, from, to, ROOK); add(l, from, to, BISHOP); add(l, from, to, KNIGHT);
    } else {
        add(l, from, to, NO_PIECE);
    }
}

void ref_generate_moves(const Position *p, MoveList *l) {
    int us = p->side, them = us ^ 1;
    bb_t occupied = p->by_color[0] | p->by_color[1];
    l->count = 0;
    for (bb_t b = p->by_color[us]; b; b &= b - 1) {
        int from = __builtin_ctzll(b), f = from % 8, r = from / 8;
        if (p->piece[from] == PAWN) {
            int dr = us == WHITE ? 1 : -1, start = us == WHITE ? 1 : 6;
            int one = from + 8 * dr;
            if (one >= 0 && one < 64 && !(occupied & BIT(one))) {
                add_pawn(l, from, one, us);
                int two = one + 8 * dr;
                if (r == start && !(occupied & BIT(two))) add(l, from, two, NO_PIECE);
            }
            for (int df = -1; df <= 1; df += 2) {
                if (!on_board(f + df, r + dr)) continue;
                int to = (r + dr) * 8 + f + df;
                if (p->by_color[them] & BIT(to)) add_pawn(l, from, to, us);
                else if (to == p->ep) add(l, from, to, NO_PIECE);
            }
            continue;
        }
        bb_t targets = ref_attacks_from(p, from) & ~p->by_color[us];
        for (; targets; targets &= targets - 1) add(l, from, __builtin_ctzll(targets), NO_PIECE);
    }
    int home = us == WHITE ? 0 : 56, king = home + 4;
    if (p->piece[king] == KING && p->color[king] == us) {
        if ((p->castle_rooks & BIT(home + 7)) && p->piece[home + 7] == ROOK && p->color[home + 7] == us &&
            !(occupied & (BIT(home + 5) | BIT(home + 6))))
            add(l, king, home + 6, NO_PIECE);
        if ((p->castle_rooks & BIT(home)) && p->piece[home] == ROOK && p->color[home] == us &&
            !(occupied & (BIT(home + 1) | BIT(home + 2) | BIT(home + 3))))
            add(l, king, home + 2, NO_PIECE);
    }
}

int ref_mover_left_king_capturable(const Position *p) {
    int k = ref_king_square(p, p->side ^ 1);
    return k < 0 || ref_is_attacked(p, k, p->side);
}

bb_t ref_visible_squares(const Position *p, int color) {
    Position view = *p;
    if (color != p->side) view.ep = -1;
    view.side = color;
    MoveList l;
    ref_generate_moves(&view, &l);
    bb_t seen = view.by_color[color];
    for (int i = 0; i < l.count; ++i) {
        int from = MOVE_FROM(l.moves[i]), to = MOVE_TO(l.moves[i]);
        seen |= BIT(to);
        if (view.piece[from] == PAWN && to == view.ep && view.piece[to] == NO_PIECE)
            seen |= BIT(color == WHITE ? to - 8 : to + 8);
    }
    return seen;
}

void ref_observation(const Position *p, int color, char out[65]) {
    bb_t seen = ref_visible_squares(p, color);
    for (int sq = 0; sq < 64; ++sq) {
        if (!(seen & BIT(sq))) out[sq] = '?';
        else out[sq] = p->piece[sq] == NO_PIECE ? '-' : symbols[(int)p->color[sq]][(int)p->piece[sq]];
    }
    out[64] = '\0';
}
