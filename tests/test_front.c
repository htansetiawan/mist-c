/* The front end's bookkeeping, each rule as White and as Black (the second
 * run mirrors the board: rank r -> 7 - r, colours swapped). Captures counted
 * (en passant too), castling rights lost, the material lead, repetitions of
 * our pieces detected, and the last-resort move taking a visible king. */
#include <stdio.h>
#include <string.h>

#include "../src/front.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL (%s): %s\n", colour, what); ++failures; } } while (0)

static const char *colour = "";
static int us = WHITE;

static int sq_of(const char *name) { return (name[0] - 'a') + 8 * (name[1] - '1'); }
/* White's-eye squares and pieces, mirrored when we play Black */
static int at(const char *name) { const int sq = sq_of(name); return us == WHITE ? sq : sq ^ 56; }
static char piece(char c) { return us == WHITE ? c : (char)(c >= 'a' ? c - 32 : c + 32); }
static Move mv(const char *uci) { return MAKE_MOVE(at(uci), at(uci + 2), NO_PIECE); }

/* A position from White's-eye placements like "Ke1 Nc3 kd8 bd5"; we are to move. */
static void setup(Position *p, const char *placements, bb_t castle, const char *ep) {
    char board[65]; memset(board, '-', 64); board[64] = '\0';
    for (const char *s = placements; *s; ) {
        while (*s == ' ') ++s;
        if (!*s) break;
        board[at(s + 1)] = piece(s[0]);
        s += 3;
    }
    pos_from_board_string(p, board, us, castle, ep ? at(ep) : -1);
}

static void run(void) {
    char view[65];
    Position p;
    Front f;
    /* captures: Nc3xd5 takes a bishop */
    setup(&p, "Ke1 Nc3 ke8 bd5", 0, NULL);
    observation(&p, us, view);
    front_init(&f, us);
    const int before = front_lead(&f, view);
    front_play(&f, view, mv("c3d5"));
    CHECK(f.their[BISHOP] == 1, "a capture of a bishop is counted");
    CHECK(front_lead(&f, view) - before == 3, "the lead grows by the bishop");
    /* en passant: e5xd6 takes the pawn that just went d7-d5 */
    setup(&p, "Ke1 Pe5 ke8 pd5", 0, "d6");
    observation(&p, us, view);
    MoveList allowed; view_moves(view, us, 0, &allowed);
    int has_ep = 0;
    for (int i = 0; i < allowed.count; ++i) has_ep |= allowed.moves[i] == mv("e5d6");
    CHECK(has_ep, "the view proves the en passant capture");
    front_init(&f, us);
    front_play(&f, view, mv("e5d6"));
    CHECK(f.their[PAWN] == 7, "an en passant capture is counted");
    /* castling rights */
    const bb_t both = ((bb_t)1 << at("a1")) | ((bb_t)1 << at("h1"));
    setup(&p, "Ke1 Ra1 Rh1 ke8", both, NULL);
    observation(&p, us, view);
    front_init(&f, us); front_see(&f, view);
    CHECK(f.castle == both, "both rooks may castle at the start");
    front_play(&f, view, mv("e1f1"));
    CHECK(f.castle == 0, "a king move ends castling");
    front_init(&f, us); front_see(&f, view);
    front_play(&f, view, mv("h1g1"));
    CHECK(f.castle == ((bb_t)1 << at("a1")), "a rook move ends castling on its side");
    setup(&p, "Ke1 Rh1 ke8", both, NULL);
    observation(&p, us, view);
    front_init(&f, us); front_see(&f, view);
    CHECK(f.castle == ((bb_t)1 << at("h1")), "a rook gone from its corner ends castling there");
    /* repetition: Ng1-f3-g1-f3-g1 while their knight shuffles (White moves first); a third Nf3 is flagged */
    pos_start(&p);
    front_init(&f, us);
    const char *ours[4] = {"g1f3", "f3g1", "g1f3", "f3g1"}, *theirs[5] = {"g8f6", "f6g8", "g8f6", "f6g8", "g8f6"};
    for (int i = 0; i < 4; ++i) {
        Undo u;
        if (us == BLACK) make_move(&p, MAKE_MOVE(at(theirs[i]), at(theirs[i] + 2), NO_PIECE), &u);
        observation(&p, us, view);
        front_play(&f, view, mv(ours[i])); make_move(&p, mv(ours[i]), &u);
        if (us == WHITE) make_move(&p, MAKE_MOVE(at(theirs[i]), at(theirs[i] + 2), NO_PIECE), &u);
    }
    if (us == BLACK) { Undo u; make_move(&p, MAKE_MOVE(at(theirs[4]), at(theirs[4] + 2), NO_PIECE), &u); }
    CHECK(p.side == us && p.piece[at("g1")] == KNIGHT && p.color[at("g1")] == us, "our knight is home and it is our move");
    observation(&p, us, view);
    MoveList moves; generate_moves(&p, &moves);
    Move out[256]; const int n = front_repeats(&f, view, &moves, out);
    int flagged = 0, other = 0;
    for (int i = 0; i < n; ++i) { flagged |= out[i] == mv("g1f3"); other |= out[i] == mv("b1c3"); }
    CHECK(flagged, "a third Nf3 is flagged as a repetition");
    CHECK(!other, "Nc3 (new) is not flagged");
    CHECK(n == 1, "exactly one move repeats");
    /* the last resort takes a visible king */
    setup(&p, "Ke1 Rh1 pg7 kh8", 0, NULL);
    observation(&p, us, view);
    view_moves(view, us, 0, &allowed);
    front_init(&f, us);
    CHECK(front_best_allowed(&f, view, &allowed) == mv("h1h8"), "the last resort takes the king");
    /* the root exclusions: the repetition rule yields when the union would leave nothing */
    {
        Move rep[256], gd[256], out[512]; int nr = 0, ng = 0, yielded = 0;
        const Move b = allowed.moves[allowed.count - 1];
        for (int i = 0; i + 1 < allowed.count; ++i) rep[nr++] = allowed.moves[i];   /* every move but b repeats */
        gd[ng++] = b;                                                                 /* the guards exclude b */
        int n = front_exclusions(&allowed, rep, nr, gd, ng, out, &yielded);
        CHECK(n == 1 && out[0] == b && yielded, "the repetition rule yields to the guards when nothing would be left");
        n = front_exclusions(&allowed, rep, 1, gd, 1, out, &yielded);
        CHECK(n == 2 && !yielded, "otherwise the exclusions are the union");
        n = front_exclusions(&allowed, rep, 1, rep, 1, out, &yielded);
        CHECK(n == 1 && !yielded, "a move in both lists is listed once");
        n = front_exclusions(&allowed, allowed.moves, allowed.count, allowed.moves, allowed.count, out, &yielded);
        CHECK(n == 0, "both lists covering everything exclude nothing");
    }
    /* the legality fallback respects the exclusions, and ignores them only when nothing is left */
    {
        Move avoid[1] = { mv("h1h8") };
        const Move m = front_best_allowed_avoiding(&f, view, &allowed, avoid, 1);
        int in = 0; for (int i = 0; i < allowed.count; ++i) in |= allowed.moves[i] == m;
        CHECK(m != mv("h1h8") && in, "the fallback skips an excluded move");
        CHECK(front_best_allowed_avoiding(&f, view, &allowed, allowed.moves, allowed.count) == mv("h1h8"), "with every move excluded the fallback ignores the exclusions");
    }
    printf("%s: done\n", colour);
}

int main(void) {
    fow_init(); zobrist_init();
    us = WHITE; colour = "as White"; run();
    us = BLACK; colour = "as Black"; run();
    printf("%s\n", failures ? "test_front: FAILED" : "test_front: passed");
    return failures ? 1 : 0;
}
