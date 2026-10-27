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
static void t_setup(Position *p, const char *placements) {
    char board[65]; memset(board, '-', 64); board[64] = '\0';
    for (const char *s = placements; *s; ) {
        while (*s == ' ') ++s;
        if (!*s) break;
        board[t_at(s + 1)] = t_piece(s[0]);
        s += 3;
    }
    pos_from_board_string(p, board, t_us, 0, -1);
}
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
    t_setup(&ws[0], "Kf1 Qd4 Ra2 Rf2 kh8 rf8 ng4");
    t_setup(&ws[1], "Kf1 Qd4 Ra2 Rf2 kh8 bf6 nc3");
    t_setup(&ws[2], "Kf1 Qd4 Ra2 Rf2 kh8 bb1");
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

int main(void) {
    fow_init();
    for (t_us = WHITE; t_us <= BLACK; ++t_us) { rule_tests(); risk_tests(); printf("%s: done\n", t_us == WHITE ? "as White" : "as Black"); }
    if (failures) { printf("test_rule: %d failures\n", failures); return 1; }
    printf("test_rule: passed\n");
    return 0;
}
