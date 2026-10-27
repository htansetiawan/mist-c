/* The root guard rule and the three risk terms, tested through main.c's
 * static functions: main.c is included with its main() renamed, so the rule
 * (guard_rule) and the risk routines (king_risks, queen_risks, piece_risks)
 * run on fixed inputs. Run: ./build/test_rule */
#define main mist_main
#include "../src/main.c"
#undef main

static int failures = 0;
static int t_us = WHITE;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL (%s): %s\n", t_us == WHITE ? "as White" : "as Black", what); ++failures; } } while (0)

/* White's-eye squares and pieces, mirrored when we play Black (as tests/test_front.c) */
static int t_sq(const char *name) { return (name[0] - 'a') + 8 * (name[1] - '1'); }
static int t_at(const char *name) { const int sq = t_sq(name); return t_us == WHITE ? sq : sq ^ 56; }
static Move t_mv(const char *uci) { return MAKE_MOVE(t_at(uci), t_at(uci + 2), NO_PIECE); }
static char t_piece(char c) { return t_us == WHITE ? c : (char)(c >= 'a' ? c - 32 : c + 32); }
static void t_setup_ep(Position *p, const char *placements, const char *ep) {
    char board[65]; memset(board, '-', 64); board[64] = '\0';
    for (const char *s = placements; *s; ) {
        while (*s == ' ') ++s;
        if (!*s) break;
        board[t_at(s + 1)] = t_piece(s[0]);
        s += 3;
    }
    pos_from_board_string(p, board, t_us, 0, ep ? t_at(ep) : -1);
}
static void t_setup(Position *p, const char *placements) { t_setup_ep(p, placements, NULL); }
static unsigned long long t_next(unsigned long long *s) { *s += 0x9e3779b97f4a7c15ULL; unsigned long long z = *s; z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL; z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL; return z ^ (z >> 31); }
static double t_unit(unsigned long long *s) { return (double)(t_next(s) >> 11) / 9007199254740992.0; }

/* The rule on fixed risk arrays (the same under every weighting unless stated). */
static void rule_tests(void) {
    const double lam = 0.9, lamp = 0.758, guard = 0.02, uguard = -1;
    const double z[3] = {0, 0, 0};
    int out[3], kq[3]; GuardStats st;
    /* 05i9doxr:22 (the B1 fatal case): a = the king-safe move with a 20% piece risk, b = the move with a
     * 5.7% king risk and no piece risk, c = a third move with a 90% king risk */
    const double k[3] = {0.0, 0.057, 0.9}, p[3] = {0.20, 0.0, 0.0};
    guard_rule(3, k, k, k, z, z, z, p, p, p, lam, lamp, guard, uguard, out, kq, &st);
    CHECK(kq[0] == 0 && kq[1] == 1 && kq[2] == 1, "v35's rule keeps only the king-safe move");
    CHECK(out[0] == 0 && out[1] == 1 && out[2] == 1, "the piece term cannot bring back the 5.7% king risk (05i9doxr:22)");
    CHECK(0.057 < lamp * 0.20, "the counterexample is a genuine trade-off (b's combined risk is the lower one)");
    /* rjaerwc3:25: every move inside the king margin; the piece term vetoes the recorded piece-losing move */
    const double k2[3] = {0.0, 0.0094, 0.0}, p2[3] = {0.30, 0.0, 0.0};
    guard_rule(3, k2, k2, k2, z, z, z, p2, p2, p2, lam, lamp, guard, uguard, out, kq, &st);
    CHECK(kq[0] == 0 && kq[1] == 0 && kq[2] == 0, "v35's rule keeps all three (0.94% is inside the margin)");
    CHECK(out[0] == 1 && out[1] == 0 && out[2] == 0, "the piece term vetoes the piece-losing move among v35's survivors");
    /* the queen term still belongs to v35's rule: a 30% queen risk is out under king + 0.9 queen */
    const double q3[3] = {0.0, 0.30, 0.0};
    guard_rule(3, z, z, z, q3, q3, q3, z, z, z, lam, lamp, guard, uguard, out, kq, &st);
    CHECK(kq[1] == 1 && out[1] == 1 && !out[0] && !out[2], "a queen risk above the margin is out under v35's rule and stays out");
    /* on random risks: a move v35 leaves out stays out, a move is always left, and with the piece term off
     * the verdict is v35's; with and without the uniform clause */
    unsigned long long rng = 12345;
    int subset_ok = 1, nonempty_ok = 1, disabled_ok = 1;
    for (int trial = 0; trial < 5000; ++trial) {
        const int n = 2 + (int)(t_next(&rng) % 12);
        double kr[14], kt[14], ku[14], qr[14], qt[14], qu[14], pr[14], pt[14], pu[14];
        for (int i = 0; i < n; ++i) {
            kr[i] = t_unit(&rng) * 0.2; kt[i] = t_unit(&rng) * 0.2; ku[i] = t_unit(&rng) * 0.2;
            qr[i] = t_unit(&rng) * 0.3; qt[i] = t_unit(&rng) * 0.3; qu[i] = t_unit(&rng) * 0.3;
            pr[i] = t_unit(&rng) * 0.4; pt[i] = t_unit(&rng) * 0.4; pu[i] = t_unit(&rng) * 0.4;
        }
        const double ug = (trial & 1) ? 0.05 : -1;
        int o[14], k0[14], o0[14], k1[14];
        guard_rule(n, kr, kt, ku, qr, qt, qu, pr, pt, pu, lam, lamp, guard, ug, o, k0, &st);
        guard_rule(n, kr, kt, ku, qr, qt, qu, pr, pt, pu, lam, 0.0, guard, ug, o0, k1, &st);
        int all = 1;
        for (int i = 0; i < n; ++i) {
            if (k0[i] && !o[i]) subset_ok = 0;
            if (!o[i]) all = 0;
            if (o0[i] != k0[i] || k1[i] != k0[i]) disabled_ok = 0;
        }
        if (all) nonempty_ok = 0;
    }
    CHECK(subset_ok, "a move v35 leaves out stays out (the allowed set is a subset of v35's)");
    CHECK(nonempty_ok, "the rule always leaves a move");
    CHECK(disabled_ok, "with the piece term off the verdict is v35's");
    /* the sharpened-only fallback among v35's survivors: a is far above b under the fitted weighting and
     * b far above a under the sharpened one, so both are out in the first pass; the sharpened one decides */
    const double kf[3] = {0.0, 0.0, 0.9};
    const double pf[3] = {0.30, 0.0, 0.0}, ps[3] = {0.0, 0.30, 0.0};
    guard_rule(3, kf, kf, kf, z, z, z, pf, ps, pf, lam, lamp, guard, uguard, out, kq, &st);
    CHECK(out[2] == 1 && out[0] == 0 && out[1] == 1, "when both weightings together leave nothing, the sharpened one alone decides");
}

/* The three risk terms on a fixed belief of three worlds that share our pieces
 * (Kf1 Qd4 Ra2 Rf2): one where f2h2 exposes the king to a rook on f8 and hangs
 * the rook to a knight, one where the queen is lost to a bishop and the a2 rook
 * hangs to a knight, one where only the a2 rook can hang. */
static void risk_tests(void) {
    Position ws[3];
    /* the pawns g7 and h7 close d4-h8 and the h-file to the king: without them our queen, or
     * the rook after f2h2, already attacks the king, and every enemy capture that leaves it so
     * is a suicidal one that the terminal-aware helpers do not charge (terminal_tests) */
    t_setup(&ws[0], "Kf1 Qd4 Ra2 Rf2 kh8 rf8 ng4 pg7");
    t_setup(&ws[1], "Kf1 Qd4 Ra2 Rf2 kh8 bf6 nc3 ph7");
    t_setup(&ws[2], "Kf1 Qd4 Ra2 Rf2 kh8 bb1 pg7");
    Belief b; memset(&b, 0, sizeof b);
    b.items = ws; b.count = 3; b.us = t_us;
    double k1, kt, ku, q1, qt, qu, p1, pt, pu;
    const double third = 1.0 / 3.0, eps = 1e-9;
    const Move m1 = t_mv("f2h2"), m2 = t_mv("f2f3");
    king_risks(&b, m1, 20000, 2.0, &k1, &kt, &ku);
    queen_risks(&b, m1, 20000, 2.0, 4, &q1, &qt, &qu);
    piece_risks(&b, m1, 20000, 2.0, 3, 4, &p1, &pt, &pu);
    CHECK(fabs(k1 - third) < eps && fabs(kt - third) < eps && fabs(ku - third) < eps, "f2h2: the king is capturable in the rook-f8 world (1/3)");
    CHECK(fabs(q1 - third) < eps, "f2h2: the queen is lost in the bishop-f6 world (1/3)");
    CHECK(fabs(p1) < eps && fabs(pt) < eps && fabs(pu) < eps, "f2h2: the piece term counts neither the king's world (rook hangs there too) nor the queen's world (rook hangs there too)");
    king_risks(&b, m2, 20000, 2.0, &k1, &kt, &ku);
    queen_risks(&b, m2, 20000, 2.0, 4, &q1, &qt, &qu);
    piece_risks(&b, m2, 20000, 2.0, 3, 4, &p1, &pt, &pu);
    CHECK(fabs(k1) < eps, "f2f3: the king stays covered");
    CHECK(fabs(q1 - third) < eps, "f2f3: the queen is lost in the bishop-f6 world (1/3)");
    CHECK(fabs(p1 - 2 * third) < eps && fabs(pt - 2 * third) < eps && fabs(pu - 2 * third) < eps, "f2f3: a rook hangs for 5 in the two worlds not charged to the queen (2/3)");
    /* the three events are disjoint on every move: their shares never add past one */
    MoveList l; generate_moves(&ws[0], &l);
    int disjoint = 1;
    for (int i = 0; i < l.count; ++i) {
        king_risks(&b, l.moves[i], 20000, 2.0, &k1, &kt, &ku);
        queen_risks(&b, l.moves[i], 20000, 2.0, 4, &q1, &qt, &qu);
        piece_risks(&b, l.moves[i], 20000, 2.0, 3, 4, &p1, &pt, &pu);
        if (k1 + q1 + p1 > 1 + eps || kt + qt + pt > 1 + eps || ku + qu + pu > 1 + eps) disjoint = 0;
    }
    CHECK(l.count > 20 && disjoint, "king, queen and piece shares are of disjoint events on every move");
}

/* Move accounting at the edges of the risk routines (test-only fixtures, as
 * Astra's review asked): taking the opponent's king, en passant, promotion. */
static void edge_tests(void) {
    Belief b; memset(&b, 0, sizeof b);
    Position w; b.items = &w; b.count = 1; b.us = t_us;
    double k1, kt, ku, q1, qt, qu, p1, pt, pu;
    const double eps = 1e-9;
    /* 1. taking the opponent's king is the won world: no king, queen or piece event is charged, even
     *    though our rook would then hang to the bishop and our queen to the other rook */
    t_setup(&w, "Kg1 Qd4 Rh1 kh8 bf6 ra4");
    king_risks(&b, t_mv("h1h8"), 20000, 2.0, &k1, &kt, &ku);
    queen_risks(&b, t_mv("h1h8"), 20000, 2.0, 4, &q1, &qt, &qu);
    piece_risks(&b, t_mv("h1h8"), 20000, 2.0, 3, 4, &p1, &pt, &pu);
    CHECK(fabs(k1) < eps && fabs(q1) < eps && fabs(p1) < eps, "taking the king charges no king, queen or piece event");
    /* 2. en passant: the captured pawn credits one pawn, which takes a would-be net-3 minor loss
     *    below the threshold (knight c3 hangs to the bishop b4 with no recapture) */
    t_setup_ep(&w, "Ke1 Nc3 Pe5 ke8 pd5 bb4", "d6");
    piece_risks(&b, t_mv("e5e6"), 20000, 2.0, 3, 4, &p1, &pt, &pu);
    CHECK(fabs(p1 - 1.0) < eps, "the pawn push leaves the knight hanging for 3: a piece event");
    piece_risks(&b, t_mv("e5d6"), 20000, 2.0, 3, 4, &p1, &pt, &pu);
    CHECK(fabs(p1) < eps, "the en-passant capture credits the taken pawn: net 2, no piece event");
    king_risks(&b, t_mv("e5d6"), 20000, 2.0, &k1, &kt, &ku);
    CHECK(fabs(k1) < eps, "en passant leaves the king covered (the knight still blocks b4-e1)");
    /* 3. promotion: make_move puts the promoted queen on the eighth rank; the risk routines then see it.
     *    The routines credit captures only, not the promotion's own material gain: a new queen taken
     *    at once by the rook a8 is charged as a queen loss of 9 (the queen's world; not the piece's) */
    t_setup(&w, "Kg1 Pe7 kh6 ra8");
    {
        Position q = w; Undo u; make_move(&q, MAKE_MOVE(t_at("e7"), t_at("e8"), QUEEN), &u);
        CHECK(q.piece[t_at("e8")] == QUEEN && q.color[t_at("e8")] == t_us && q.piece[t_at("e7")] == NO_PIECE, "promotion puts our queen on e8");
    }
    queen_risks(&b, MAKE_MOVE(t_at("e7"), t_at("e8"), QUEEN), 20000, 2.0, 4, &q1, &qt, &qu);
    piece_risks(&b, MAKE_MOVE(t_at("e7"), t_at("e8"), QUEEN), 20000, 2.0, 3, 4, &p1, &pt, &pu);
    king_risks(&b, MAKE_MOVE(t_at("e7"), t_at("e8"), QUEEN), 20000, 2.0, &k1, &kt, &ku);
    CHECK(fabs(q1 - 1.0) < eps && fabs(p1) < eps && fabs(k1) < eps, "a promoted queen taken at once is the queen term's event (captures credited, promotion gain not)");
}

/* Terminal awareness of the piece helper (Astra, 27 Oct, from the r3 bench:
 * v36r3-vs-misty pair 0 as White, decision 28). An enemy capture that leaves
 * the enemy king capturable ends the game in our favour on the next ply, so it
 * is no loss of the piece: best_capture_net must skip it, whether the capturer
 * is the king itself or a piece whose capture opens its king. Captures that
 * are safe for the capturer stay charged. */
static void terminal_tests(void) {
    Belief b; memset(&b, 0, sizeof b);
    Position w; b.items = &w; b.count = 1; b.us = t_us;
    double k1, kt, ku, q1, qt, qu, p1, pt, pu;
    const double eps = 1e-9;
    /* 1. the bench board: our rook d7 is attacked only by the king e8. Rb1-d1 defends it, so
     *    Kxd7 would lose the king to Rxd7: no piece event. Rb1-c1 leaves d7 undefended: Kxd7
     *    is safe and the rook is lost. Rxb7 takes a pawn into the queen's reach for a net loss
     *    of 4 under recapture: no event. The played move was Rxb7; the vetoed one Rd1. */
    t_setup(&w, "Kg1 Rb1 Rd7 Bf4 Pa2 Pf2 Pg2 Ph3 Pe4 ke8 rc8 qa6 bc5 pb7 pf7 pg6 ph4");
    {
        Position q = w; Undo u, v; make_move(&q, t_mv("b1d1"), &u);
        CHECK(!mover_left_king_capturable(&q), "Rd1 leaves our king covered");
        make_move(&q, t_mv("e8d7"), &v);
        CHECK(mover_left_king_capturable(&q), "after Rd1, Kxd7 leaves the king to Rxd7");
    }
    const Move rd1 = t_mv("b1d1"), rxb7 = t_mv("d7b7"), rc1 = t_mv("b1c1");
    const Move three[3] = {rd1, rxb7, rc1};
    double kr[3], krt[3], kru[3], qr[3], qrt[3], qru[3], pr[3], prt[3], pru[3];
    for (int i = 0; i < 3; ++i) {
        king_risks(&b, three[i], 20000, 2.0, &kr[i], &krt[i], &kru[i]);
        queen_risks(&b, three[i], 20000, 2.0, 4, &qr[i], &qrt[i], &qru[i]);
        piece_risks(&b, three[i], 20000, 2.0, 3, 4, &pr[i], &prt[i], &pru[i]);
    }
    CHECK(fabs(pr[0]) < eps && fabs(prt[0]) < eps && fabs(pru[0]) < eps, "Rd1: the king's capture of the defended rook is its own loss, not a piece event");
    CHECK(fabs(pr[1]) < eps, "Rxb7: the rook taken back by the queen nets -4, no piece event");
    CHECK(fabs(pr[2] - 1.0) < eps, "Rc1: the undefended rook d7 falls to the king for 5, a piece event");
    CHECK(fabs(kr[0]) < eps && fabs(qr[0]) < eps && fabs(kr[1]) < eps && fabs(qr[1]) < eps, "neither move risks the king or the queen on this board");
    {
        int out[3], kq[3]; GuardStats st;
        guard_rule(3, kr, krt, kru, qr, qrt, qru, pr, prt, pru, 0.9, 0.758, 0.02, -1, out, kq, &st);
        CHECK(!kq[0] && !kq[1], "v35's king + queen rule keeps Rd1 and Rxb7");
        CHECK(!out[0] && !out[1] && out[2], "with the piece term Rd1 and Rxb7 stay in and Rc1 is out");
    }
    /* 2. a non-king capture that opens its own king: the knight e6 is pinned to e8 by our rook e1.
     *    Nxd4 would win the undefended bishop but leaves the king to Rxe8: no piece event after a
     *    quiet move of ours. With our rook on a1 instead the knight is free and the bishop is lost. */
    t_setup(&w, "Kg1 Ph2 Re1 Bd4 ke8 ne6");
    {
        Position q = w; Undo u, v; make_move(&q, t_mv("h2h3"), &u);
        make_move(&q, t_mv("e6d4"), &v);
        CHECK(mover_left_king_capturable(&q), "after h3, Nxd4 opens the e-file to the king");
    }
    piece_risks(&b, t_mv("h2h3"), 20000, 2.0, 3, 4, &p1, &pt, &pu);
    CHECK(fabs(p1) < eps && fabs(pt) < eps && fabs(pu) < eps, "a pinned knight's capture of the bishop is no piece event");
    t_setup(&w, "Kg1 Ph2 Ra1 Bd4 ke8 ne6");
    piece_risks(&b, t_mv("h2h3"), 20000, 2.0, 3, 4, &p1, &pt, &pu);
    CHECK(fabs(p1 - 1.0) < eps, "the free knight's capture of the undefended bishop is a piece event");
    /* 3. the queen's world is found through the same helper: the knight e6, pinned to e8 by our
     *    rook e1, attacks our queen d4. Nxd4 would open the king to Rxe8, so this is no queen's
     *    world, and the rook a5 hanging to the bishop c7 is charged to the piece term. (With the
     *    older accounting the queen's capture made it the queen's world and the rook went uncounted.) */
    t_setup(&w, "Kg1 Ph2 Re1 Qd4 Ra5 ke8 ne6 bc7");
    piece_risks(&b, t_mv("h2h3"), 20000, 2.0, 3, 4, &p1, &pt, &pu);
    CHECK(fabs(p1 - 1.0) < eps, "the pinned knight's capture of the queen is no queen's world: the rook a5 hanging to the bishop is the piece term's event");
    queen_risks(&b, t_mv("h2h3"), 20000, 2.0, 4, &q1, &qt, &qu);
    CHECK(fabs(q1) < eps, "the queen term agrees: the pinned knight's capture is no queen event");
    /* 4. the queen helper on the same predicate (Astra's corner): our queen arrives next to the king,
     *    defended by the rook d1, while our knight f8 stands where the king can take it safely. Kxd7
     *    is suicidal, Kxf8 is not. Queen 0, piece 1: one event on the world. With the queen helper
     *    on the older accounting the world would be charged twice (queen 1 for Kxd7, piece 1 for Kxf8). */
    t_setup(&w, "Kg1 Rd1 Qd4 Nf8 ke8");
    queen_risks(&b, t_mv("d4d7"), 20000, 2.0, 4, &q1, &qt, &qu);
    piece_risks(&b, t_mv("d4d7"), 20000, 2.0, 3, 4, &p1, &pt, &pu);
    king_risks(&b, t_mv("d4d7"), 20000, 2.0, &k1, &kt, &ku);
    CHECK(fabs(q1) < eps && fabs(qt) < eps && fabs(qu) < eps, "Qd7 defended by Rd1: the king's capture is no queen event");
    CHECK(fabs(p1 - 1.0) < eps, "the knight f8 the king takes safely is the piece term's event");
    CHECK(k1 + q1 + p1 < 1 + eps, "one event on the world, not two");
    t_setup(&w, "Kg1 Ra1 Qd4 ke8");
    queen_risks(&b, t_mv("d4d7"), 20000, 2.0, 4, &q1, &qt, &qu);
    CHECK(fabs(q1 - 1.0) < eps, "Qd7 undefended: the king takes it safely, a queen event");
}

int main(void) {
    fow_init();
    for (t_us = WHITE; t_us <= BLACK; ++t_us) { rule_tests(); risk_tests(); edge_tests(); terminal_tests(); printf("%s: done\n", t_us == WHITE ? "as White" : "as Black"); }
    if (failures) { printf("test_rule: %d failures\n", failures); return 1; }
    printf("test_rule: passed\n");
    return 0;
}
