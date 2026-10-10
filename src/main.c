/* mist-c: referee-protocol front end (milestone M5, first version).
 *
 * The course referee sends "fow", then "<w|b> <base> <increment>"; the bot
 * answers "ready". Then a 64-character view arrives before every ply, and our
 * remaining seconds when it is our turn; we answer "move <uci>". The exact
 * belief is updated from each view: the first view is the start (nothing
 * moved), a view following our own move is updated with that move, any other
 * view by every opponent move consistent with it. If the capped belief ever
 * loses every position, it is rebuilt from the current view (belief_resample),
 * with the opponent's material counted from our captures. Whatever decides,
 * the move sent must be one view_moves allows: v1 lost two games by pushing a
 * pawn into an unseen piece after its belief ran empty.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eval.h"
#include "gtcfr.h"
#include "search.h"

#define BIT_AT(sq) ((bb_t)1 << (sq))

static int is_view(const char *s) {
    if (strlen(s) != 64) return 0;
    for (int i = 0; i < 64; ++i) if (!strchr("PNBRQKpnbrqk-?", s[i])) return 0;
    return 1;
}

/* What we see, as a position: our pieces and the visible enemy pieces. */
static void view_position(const char *view, int us, Position *p, bb_t castle) {
    char board[65];
    for (int i = 0; i < 64; ++i) board[i] = view[i] == '?' ? '-' : view[i];
    board[64] = '\0';
    pos_from_board_string(p, board, us, castle, -1);
}

static int piece_index(char c) {
    const char *at = strchr("pnbrqk", c >= 'A' && c <= 'Z' ? c + 32 : c);
    return c && at ? (int)(at - "pnbrqk") : -1;
}

/* The best move view_moves allows, by the evaluation after it on the visible
 * position: the last resort when nothing else gave a move it allows. */
static Move best_view_move(const char *view, int us, bb_t castle, const MoveList *allowed) {
    Position seen; view_position(view, us, &seen, castle);
    Move best = allowed->moves[0]; int best_cp = -1000000;
    for (int i = 0; i < allowed->count; ++i) {
        Position q = seen; Undo u; make_move(&q, allowed->moves[i], &u);
        const int cp = king_square(&q, us ^ 1) < 0 ? 100000 : evaluate_cp(&q, us);
        if (cp > best_cp) { best_cp = cp; best = allowed->moves[i]; }
    }
    return best;
}

int main(void) {
    char line[256];
    if (!fgets(line, sizeof line, stdin) || strncmp(line, "fow", 3) != 0) return 0;
    char side[4]; double base = 180, increment = 2;
    if (!fgets(line, sizeof line, stdin) || sscanf(line, "%3s %lf %lf", side, &base, &increment) < 1) return 0;
    const int us = side[0] == 'w' ? WHITE : BLACK;
    fow_init(); zobrist_init();
    const size_t cap = getenv("MIST_CAP") ? (size_t)strtoull(getenv("MIST_CAP"), NULL, 10) : 200000;
    Belief b; belief_init_start(&b, us, cap);
    printf("ready\n"); fflush(stdout);

    char view[65] = "";
    int views = 0, moved = 0, decisions = 0, rebuilds = 0;
    Move last = 0;
    /* what the opponent still has (less our captures) and the rooks we may castle with */
    int their[6] = {8, 2, 2, 2, 1, 1};
    const int home = us == WHITE ? 0 : 56;
    bb_t our_castle = BIT_AT(home) | BIT_AT(home + 7);
    const double max_seconds = getenv("MIST_MAX_SECONDS") ? atof(getenv("MIST_MAX_SECONDS")) : 10.0;
    unsigned long long seed = getenv("FOW_SEED") ? strtoull(getenv("FOW_SEED"), NULL, 10) : 1;
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (is_view(line)) {
            memcpy(view, line, 65);
            if (views++ > 0 && b.count > 0) {
                if (moved) belief_after_own(&b, last, view);
                else belief_after_opponent(&b, view);
            }
            /* a rook gone from its corner (or a king gone from its square) ends castling there */
            const char rook = us == WHITE ? 'R' : 'r', king = us == WHITE ? 'K' : 'k';
            if (view[home + 4] != king) our_castle = 0;
            if (view[home] != rook) our_castle &= ~BIT_AT(home);
            if (view[home + 7] != rook) our_castle &= ~BIT_AT(home + 7);
            if (b.count == 0 && !moved && views > 1) {
                /* our turn and nothing left: rebuild from what we see */
                const size_t n = belief_resample(&b, view, their, our_castle, 2000, 60000);
                fprintf(stderr, "mist: belief empty, rebuilt %zu positions from the view (rebuild %d)\n", n, ++rebuilds);
            }
            moved = 0;
            continue;
        }
        char *end; const double remaining = strtod(line, &end);
        if (end == line) continue;
        /* our turn: about remaining/30 + 80% of the increment, capped at 10 s;
         * a larger sample of P when there is time for it */
        double budget = remaining / 30.0 + 0.8 * increment;
        if (budget > max_seconds) budget = max_seconds;
        if (budget > remaining * 0.3) budget = remaining * 0.3;
        SearchParams sp = {budget >= 3.0 ? 256 : 128, 2000, seed + (unsigned long long)decisions, budget * 0.6, 0};
        if (remaining < 20) { sp.samples = 32; sp.iterations = 60; sp.seconds = 0.3; }
        SearchResult r; int ok = 0;
        /* GT-CFR by default (MIST_SEARCH=twoply for the fixed two-ply search) */
        const int twoply = getenv("MIST_SEARCH") && strcmp(getenv("MIST_SEARCH"), "twoply") == 0;
        GtcfrParams gp = {budget >= 3.0 ? 256 : 128, budget, 1500000, 32, 10, seed + (unsigned long long)decisions};
        if (remaining < 20) { gp.samples = 32; gp.seconds = 0.3; }
        if (b.count > 0) ok = twoply ? search_decide(&b, &sp, &r) : gtcfr_decide(&b, &gp, &r);
        if (!ok) {
            /* fallback: decide on the position we can see */
            Position seen; view_position(view, us, &seen, b.count ? b.items[0].castle_rooks : 0);
            Belief one = {0}; one.items = &seen; one.count = 1; one.us = us;
            ok = search_decide(&one, &sp, &r);
            fprintf(stderr, "mist: belief empty, decided on the visible position\n");
        }
        MoveList allowed;
        if (view_moves(view, us, our_castle, &allowed) > 0) {
            int found = 0;
            for (int i = 0; i < allowed.count; ++i) found |= allowed.moves[i] == r.move;
            if (!ok || !found) {
                char bad[6]; move_to_uci(r.move, bad);
                r.move = best_view_move(view, us, our_castle, &allowed);
                fprintf(stderr, "mist: %s is not safe on every board the view allows; playing a move it allows\n", ok ? bad : "(none)");
                ok = 1;
            }
        }
        if (!ok) { fprintf(stderr, "mist: no legal move\n"); break; }
        char uci[6]; move_to_uci(r.move, uci);
        {
            /* count our capture (the destination is visible), en passant included */
            const int to = MOVE_TO(r.move), from = MOVE_FROM(r.move);
            const int taken = piece_index(view[to]), mover = piece_index(view[from]);
            const int theirs = view[to] != '-' && view[to] != '?' && ((us == WHITE) == (view[to] >= 'a' && view[to] <= 'z'));
            if (theirs && taken >= 0 && their[taken] > 0) --their[taken];
            else if (mover == PAWN && view[to] == '-' && (to - from) % 8 != 0 && their[PAWN] > 0) --their[PAWN];
            if (mover == KING) our_castle = 0;
            our_castle &= ~(BIT_AT(from) | BIT_AT(to));
        }
        fprintf(stderr, "mist: n=%d |P|=%zu%s samples=%d moves=%d infosets=%d nodes=%d iters=%d p=%.2f v=%+.3f clock=%.1f move=%s\n",
                ++decisions, b.count, b.overflowed ? "(capped)" : "", r.samples, r.our_moves, r.opponent_infosets, r.nodes,
                r.iterations, r.probability, r.value, remaining, uci);
        printf("move %s\n", uci); fflush(stdout);
        last = r.move; moved = 1;
    }
    belief_free(&b);
    return 0;
}
