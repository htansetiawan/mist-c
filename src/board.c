/* mist-c: Fog of War chess rules (milestone M1). See fow.h. */
#include "fow.h"

#include <string.h>

/* Tables, filled by fow_init: knight and king steps, the squares a pawn of each
 * colour attacks, and the eight rays from each square (0..3 point up the
 * board or to the right, so the first blocker is the lowest set bit; 4..7 the
 * other way, the highest). Sliding attacks are the ray up to and including
 * its first blocker; from the target's side the same rays find the attackers,
 * so an attack test no longer visits every piece. */
static bb_t knight_table[64], king_table[64], pawn_attack_table[2][64], ray_table[8][64];
static const int ray_df[8] = {0, 1, 1, -1, 0, -1, 1, -1};
static const int ray_dr[8] = {1, 0, 1, 1, -1, 0, -1, -1};
static const char symbols[2][7] = {"PNBRQK", "pnbrqk"};

#define BIT(sq) ((bb_t)1 << (sq))
#define FILE_A ((bb_t)0x0101010101010101ULL)
#define FILE_H ((bb_t)0x8080808080808080ULL)
#define RANK_3 ((bb_t)0x0000000000FF0000ULL)
#define RANK_6 ((bb_t)0x0000FF0000000000ULL)

static int on_board(int file, int rank) { return file >= 0 && file < 8 && rank >= 0 && rank < 8; }

void fow_init(void) {
    static const int kn[8][2] = {{1, 2}, {2, 1}, {-1, 2}, {-2, 1}, {1, -2}, {2, -1}, {-1, -2}, {-2, -1}};
    for (int sq = 0; sq < 64; ++sq) {
        int f = sq % 8, r = sq / 8;
        knight_table[sq] = king_table[sq] = 0;
        for (int i = 0; i < 8; ++i)
            if (on_board(f + kn[i][0], r + kn[i][1])) knight_table[sq] |= BIT((r + kn[i][1]) * 8 + f + kn[i][0]);
        for (int df = -1; df <= 1; ++df)
            for (int dr = -1; dr <= 1; ++dr)
                if ((df || dr) && on_board(f + df, r + dr)) king_table[sq] |= BIT((r + dr) * 8 + f + df);
        for (int color = 0; color < 2; ++color) {
            const int dr = color == WHITE ? 1 : -1;
            pawn_attack_table[color][sq] = 0;
            if (on_board(f - 1, r + dr)) pawn_attack_table[color][sq] |= BIT((r + dr) * 8 + f - 1);
            if (on_board(f + 1, r + dr)) pawn_attack_table[color][sq] |= BIT((r + dr) * 8 + f + 1);
        }
        for (int d = 0; d < 8; ++d) {
            ray_table[d][sq] = 0;
            for (int x = f + ray_df[d], y = r + ray_dr[d]; on_board(x, y); x += ray_df[d], y += ray_dr[d])
                ray_table[d][sq] |= BIT(y * 8 + x);
        }
    }
}

/* The ray from `sq` in direction `d` up to and including its first occupied square. */
static bb_t ray_attacks(int sq, int d, bb_t occupied) {
    const bb_t ray = ray_table[d][sq], block = ray & occupied;
    if (!block) return ray;
    const int first = d < 4 ? __builtin_ctzll(block) : 63 - __builtin_clzll(block);
    return ray ^ ray_table[d][first];
}

static bb_t rook_attacks(int sq, bb_t occupied) {
    return ray_attacks(sq, 0, occupied) | ray_attacks(sq, 1, occupied) | ray_attacks(sq, 4, occupied) | ray_attacks(sq, 5, occupied);
}

static bb_t bishop_attacks(int sq, bb_t occupied) {
    return ray_attacks(sq, 2, occupied) | ray_attacks(sq, 3, occupied) | ray_attacks(sq, 6, occupied) | ray_attacks(sq, 7, occupied);
}

/* The squares all sliders in `gen` reach in one direction, up to and
 * including the first occupied square (`empty` = the empty squares): a
 * parallel-prefix flood in three doubling steps, the same answer as the ray
 * scan for every slider at once. The wrap masks keep file shifts on the board. */
#define FILL(name, shift_expr, mask)                                          \
    static bb_t name(bb_t gen, bb_t empty) {                                 \
        bb_t pro = empty & (mask);                                           \
        gen |= pro & shift_expr(gen, 1);                                     \
        pro &= shift_expr(pro, 1);                                           \
        gen |= pro & shift_expr(gen, 2);                                     \
        pro &= shift_expr(pro, 2);                                           \
        gen |= pro & shift_expr(gen, 4);                                     \
        return shift_expr(gen, 1) & (mask);                                  \
    }
#define SH_N(b, k) ((b) << (8 * (k)))
#define SH_S(b, k) ((b) >> (8 * (k)))
#define SH_E(b, k) ((b) << (k))
#define SH_W(b, k) ((b) >> (k))
#define SH_NE(b, k) ((b) << (9 * (k)))
#define SH_NW(b, k) ((b) << (7 * (k)))
#define SH_SE(b, k) ((b) >> (7 * (k)))
#define SH_SW(b, k) ((b) >> (9 * (k)))
/* masks: where a shifted square may land without wrapping; for the two- and
 * four-step shifts the mask is applied to the propagator, which the macro
 * narrows again at each doubling (pro &= shift(pro)) as the classic fill does */
FILL(fill_n, SH_N, ~(bb_t)0)
FILL(fill_s, SH_S, ~(bb_t)0)
FILL(fill_e, SH_E, ~FILE_A)
FILL(fill_w, SH_W, ~FILE_H)
FILL(fill_ne, SH_NE, ~FILE_A)
FILL(fill_nw, SH_NW, ~FILE_H)
FILL(fill_se, SH_SE, ~FILE_A)
FILL(fill_sw, SH_SW, ~FILE_H)

void pos_clear(Position *p) {
    memset(p, 0, sizeof(*p));
    for (int sq = 0; sq < 64; ++sq) { p->piece[sq] = NO_PIECE; p->color[sq] = -1; }
    p->ep = -1;
}

void pos_put(Position *p, int sq, int color, int type) {
    p->by_color[color] |= BIT(sq);
    p->by_type[type] |= BIT(sq);
    p->piece[sq] = (int8_t)type;
    p->color[sq] = (int8_t)color;
}

void pos_remove(Position *p, int sq) {
    if (p->piece[sq] == NO_PIECE) return;
    p->by_color[(int)p->color[sq]] &= ~BIT(sq);
    p->by_type[(int)p->piece[sq]] &= ~BIT(sq);
    p->piece[sq] = NO_PIECE;
    p->color[sq] = -1;
}

int pos_from_board_string(Position *p, const char *text, int side, bb_t castle_rooks, int ep) {
    pos_clear(p);
    for (int sq = 0; sq < 64; ++sq) {
        char c = text[sq];
        if (c == '-') continue;
        int found = 0;
        for (int color = 0; color < 2 && !found; ++color)
            for (int t = 0; t < 6; ++t)
                if (symbols[color][t] == c) { pos_put(p, sq, color, t); found = 1; break; }
        if (!found) return -1;
    }
    p->side = side;
    p->castle_rooks = castle_rooks;
    p->ep = ep;
    return 0;
}

void pos_start(Position *p) {
    pos_from_board_string(p, "RNBQKBNRPPPPPPPP--------------------------------pppppppprnbqkbnr", WHITE,
                          BIT(0) | BIT(7) | BIT(56) | BIT(63), -1);
}

void pos_to_board_string(const Position *p, char out[65]) {
    for (int sq = 0; sq < 64; ++sq) out[sq] = p->piece[sq] == NO_PIECE ? '-' : symbols[(int)p->color[sq]][(int)p->piece[sq]];
    out[64] = '\0';
}

int pos_matches_board_string(const Position *p, const char *text) {
    for (int sq = 0; sq < 64; ++sq) {
        const char c = text[sq];
        if (c == '-') { if (p->piece[sq] != NO_PIECE) return 0; continue; }
        if (p->piece[sq] == NO_PIECE || symbols[(int)p->color[sq]][(int)p->piece[sq]] != c) return 0;
    }
    return 1;
}

bb_t attacks_from(const Position *p, int sq) {
    const bb_t occupied = p->by_color[0] | p->by_color[1];
    switch (p->piece[sq]) {
        case PAWN: return pawn_attack_table[(int)p->color[sq]][sq];
        case KNIGHT: return knight_table[sq];
        case BISHOP: return bishop_attacks(sq, occupied);
        case ROOK: return rook_attacks(sq, occupied);
        case QUEEN: return bishop_attacks(sq, occupied) | rook_attacks(sq, occupied);
        case KING: return king_table[sq];
        default: return 0;
    }
}

/* From the target: a pawn of `by_color` attacks `sq` from where a pawn of the
 * other colour on `sq` would capture; knights and kings by their tables;
 * sliders along the rays from `sq`, which stop at the same blockers. */
int is_attacked(const Position *p, int sq, int by_color) {
    const bb_t them = p->by_color[by_color];
    if (pawn_attack_table[by_color ^ 1][sq] & p->by_type[PAWN] & them) return 1;
    if (knight_table[sq] & p->by_type[KNIGHT] & them) return 1;
    if (king_table[sq] & p->by_type[KING] & them) return 1;
    const bb_t occupied = p->by_color[0] | p->by_color[1];
    const bb_t rooks = (p->by_type[ROOK] | p->by_type[QUEEN]) & them;
    if (rooks && (rook_attacks(sq, occupied) & rooks)) return 1;
    const bb_t bishops = (p->by_type[BISHOP] | p->by_type[QUEEN]) & them;
    if (bishops && (bishop_attacks(sq, occupied) & bishops)) return 1;
    return 0;
}

int king_square(const Position *p, int color) {
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

void generate_moves(const Position *p, MoveList *l) {
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
        bb_t targets = attacks_from(p, from) & ~p->by_color[us];
        for (; targets; targets &= targets - 1) add(l, from, __builtin_ctzll(targets), NO_PIECE);
    }
    /* Castling: rights, empty squares between king and rook; attacks are irrelevant. */
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

void make_move(Position *p, Move m, Undo *u) {
    int from = MOVE_FROM(m), to = MOVE_TO(m), promo = MOVE_PROMO(m);
    int us = p->side, type = p->piece[from];
    u->castle_rooks = p->castle_rooks; u->ep = p->ep; u->halfmove = p->halfmove;
    u->captured_type = NO_PIECE; u->captured_color = -1; u->captured_square = -1;
    u->rook_from = u->rook_to = -1;
    int captured_sq = to;
    if (type == PAWN && to == p->ep && p->piece[to] == NO_PIECE) captured_sq = us == WHITE ? to - 8 : to + 8;
    if (p->piece[captured_sq] != NO_PIECE) {
        u->captured_type = p->piece[captured_sq]; u->captured_color = p->color[captured_sq]; u->captured_square = captured_sq;
        pos_remove(p, captured_sq);
    }
    pos_remove(p, from);
    pos_put(p, to, us, promo != NO_PIECE ? promo : type);
    if (type == KING && (to - from == 2 || from - to == 2)) {
        int home = us == WHITE ? 0 : 56;
        u->rook_from = to > from ? home + 7 : home;
        u->rook_to = to > from ? home + 5 : home + 3;
        pos_remove(p, u->rook_from);
        pos_put(p, u->rook_to, us, ROOK);
    }
    if (type == KING) p->castle_rooks &= ~(us == WHITE ? (BIT(0) | BIT(7)) : (BIT(56) | BIT(63)));
    p->castle_rooks &= ~(BIT(from) | BIT(to));
    p->ep = (type == PAWN && (to - from == 16 || from - to == 16)) ? (from + to) / 2 : -1;
    p->halfmove = (type == PAWN || u->captured_type != NO_PIECE) ? 0 : p->halfmove + 1;
    p->side ^= 1;
}

void unmake_move(Position *p, Move m, const Undo *u) {
    int from = MOVE_FROM(m), to = MOVE_TO(m), promo = MOVE_PROMO(m);
    p->side ^= 1;
    int us = p->side, type = promo != NO_PIECE ? PAWN : p->piece[to];
    if (u->rook_from >= 0) { pos_remove(p, u->rook_to); pos_put(p, u->rook_from, us, ROOK); }
    pos_remove(p, to);
    pos_put(p, from, us, type);
    if (u->captured_type != NO_PIECE) pos_put(p, u->captured_square, u->captured_color, u->captured_type);
    p->castle_rooks = u->castle_rooks; p->ep = u->ep; p->halfmove = u->halfmove;
}

int mover_left_king_capturable(const Position *p) {
    int k = king_square(p, p->side ^ 1);
    return k < 0 || is_attacked(p, k, p->side);
}

/* The squares `color` observes: its own pieces and the destinations of its
 * legal moves (generate_moves, with en passant only for the side to move),
 * plus the pawn an en passant capture would take. Computed as bitboards, in
 * the same terms as the move generator: pawn pushes onto empty squares, a
 * double push from the start rank through an empty square, pawn captures of
 * enemy pieces or of the en passant square, the step and sliding attacks of
 * the pieces, and the castling destinations. Own squares are seen anyway, so
 * attacks on own pieces need no masking. */
bb_t visible_squares(const Position *p, int color) {
    const bb_t own = p->by_color[color], enemy = p->by_color[color ^ 1], occupied = own | enemy;
    const int ep = color == p->side ? p->ep : -1;
    bb_t seen = own;
    const bb_t pawns = p->by_type[PAWN] & own;
    if (pawns) {
        bb_t one, two, captures;
        if (color == WHITE) {
            one = (pawns << 8) & ~occupied;
            two = ((one & RANK_3) << 8) & ~occupied;
            captures = ((pawns & ~FILE_A) << 7) | ((pawns & ~FILE_H) << 9);
        } else {
            one = (pawns >> 8) & ~occupied;
            two = ((one & RANK_6) >> 8) & ~occupied;
            captures = ((pawns & ~FILE_A) >> 9) | ((pawns & ~FILE_H) >> 7);
        }
        seen |= one | two | (captures & enemy);
        if (ep >= 0 && (captures & BIT(ep))) {
            seen |= BIT(ep);
            if (p->piece[ep] == NO_PIECE) seen |= BIT(color == WHITE ? ep - 8 : ep + 8);
        }
    }
    for (bb_t b = p->by_type[KNIGHT] & own; b; b &= b - 1) seen |= knight_table[__builtin_ctzll(b)];
    const bb_t empty = ~occupied, rooks = (p->by_type[ROOK] | p->by_type[QUEEN]) & own, bishops = (p->by_type[BISHOP] | p->by_type[QUEEN]) & own;
    if (rooks) seen |= fill_n(rooks, empty) | fill_s(rooks, empty) | fill_e(rooks, empty) | fill_w(rooks, empty);
    if (bishops) seen |= fill_ne(bishops, empty) | fill_nw(bishops, empty) | fill_se(bishops, empty) | fill_sw(bishops, empty);
    for (bb_t b = p->by_type[KING] & own; b; b &= b - 1) seen |= king_table[__builtin_ctzll(b)];
    const int home = color == WHITE ? 0 : 56, king = home + 4;
    if (p->piece[king] == KING && p->color[king] == color) {
        if ((p->castle_rooks & BIT(home + 7)) && p->piece[home + 7] == ROOK && p->color[home + 7] == color &&
            !(occupied & (BIT(home + 5) | BIT(home + 6))))
            seen |= BIT(home + 6);
        if ((p->castle_rooks & BIT(home)) && p->piece[home] == ROOK && p->color[home] == color &&
            !(occupied & (BIT(home + 1) | BIT(home + 2) | BIT(home + 3))))
            seen |= BIT(home + 2);
    }
    return seen;
}

void observation(const Position *p, int color, char out[65]) {
    bb_t seen = visible_squares(p, color);
    for (int sq = 0; sq < 64; ++sq) {
        if (!(seen & BIT(sq))) out[sq] = '?';
        else out[sq] = p->piece[sq] == NO_PIECE ? '-' : symbols[(int)p->color[sq]][(int)p->piece[sq]];
    }
    out[64] = '\0';
}

void move_to_uci(Move m, char out[6]) {
    int from = MOVE_FROM(m), to = MOVE_TO(m), promo = MOVE_PROMO(m);
    out[0] = (char)('a' + from % 8); out[1] = (char)('1' + from / 8);
    out[2] = (char)('a' + to % 8); out[3] = (char)('1' + to / 8);
    out[4] = promo != NO_PIECE ? "pnbrqk"[promo] : '\0';
    out[5] = '\0';
}
