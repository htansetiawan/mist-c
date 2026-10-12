#define _POSIX_C_SOURCE 199309L  /* clock_gettime under -std=c11 */
/* mist-c: referee-protocol front end (milestone M5).
 *
 * The course referee sends "fow", then "<w|b> <base> <increment>"; the bot
 * answers "ready". Then a 64-character view arrives before every ply, and our
 * remaining seconds when it is our turn; we answer "move <uci>". The exact
 * belief is updated from each view: the first view is the start (nothing
 * moved), a view following our own move is updated with that move, any other
 * view by every opponent move consistent with it. If the capped belief ever
 * loses every position, it is rebuilt from the current view (belief_resample),
 * with the opponent's material counted from our captures. Whatever decides,
 * the move sent must be one view_moves allows (or, with the belief exact,
 * one legal on every board in it): v1 lost two games by pushing a pawn into an
 * unseen piece after its belief ran empty. When ahead, moves that would make
 * our pieces stand as they already have twice are left out: v1 drew three
 * games by repetition, two of them seven points up. The opponent update is
 * paid from our clock: P is first thinned to what half the move's budget can
 * expand (v3 lost three games on time, its 200,000-position updates taking
 * 3-4 s against a 2 s increment).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "front.h"
#include "gtcfr.h"
#include "search.h"
#include "sfeval.h"

static int is_view(const char *s) {
    if (strlen(s) != 64) return 0;
    for (int i = 0; i < 64; ++i) if (!strchr("PNBRQKpnbrqk-?", s[i])) return 0;
    return 1;
}

/* Legal in every position of P: with P exact (never capped) the true board is
 * among them, so this proves legality where the view alone cannot (an en
 * passant whose taken pawn another piece of ours also attacks). */
static int legal_everywhere(const Belief *b, Move m) {
    for (size_t i = 0; i < b->count; ++i) {
        MoveList l; generate_moves(&b->items[i], &l);
        int found = 0;
        for (int j = 0; j < l.count && !found; ++j) found = l.moves[j] == m;
        if (!found) return 0;
    }
    return b->count > 0;
}

/* The weight share of P on which `m` leaves our king capturable (the view
 * proves `m` legal on every board of P, so it can be played on each). */
static double king_risk(const Belief *b, Move m) {
    double total = 0, lost = 0;
    for (size_t i = 0; i < b->count; ++i) {
        const double w = b->logw ? exp((double)b->logw[i]) : 1.0;
        Position p = b->items[i];
        Undo u;
        make_move(&p, m, &u);
        total += w;
        if (mover_left_king_capturable(&p)) lost += w;
    }
    return total > 0 ? lost / total : 0;
}

/* Expected material `m` loses on the next ply, in pawns, over at most `cap`
 * boards of P evenly strided (weights kept): the opponent's best immediate
 * capture (the piece taken less the capturer where we can take it back)
 * less what `m` itself takes on that board. King captures are the king
 * guard's business and count 0 here. Of 283 decisive bad moves in
 * our v25-v33 losses, 273 lost material to a piece the search's 32 sampled
 * worlds did not hold often enough; all of P sees it more often. */
static const int piece_pawns[6] = {1, 3, 3, 5, 9, 0};
static double material_risk(const Belief *b, Move m, size_t cap) {
    const size_t stride = b->count > cap ? (b->count + cap - 1) / cap : 1;
    double total = 0, lost = 0;
    for (size_t i = 0; i < b->count; i += stride) {
        const double w = b->logw ? exp((double)b->logw[i]) : 1.0;
        Position p = b->items[i];
        int got = p.piece[MOVE_TO(m)] != NO_PIECE && p.color[MOVE_TO(m)] != p.side ? piece_pawns[p.piece[MOVE_TO(m)]] : 0;
        if (!got && p.piece[MOVE_FROM(m)] == PAWN && MOVE_TO(m) == p.ep) got = 1;
        Undo u;
        make_move(&p, m, &u);
        total += w;
        if (mover_left_king_capturable(&p)) continue;
        const int us = p.side ^ 1;
        MoveList l; generate_moves(&p, &l);
        int best = 0;
        for (int j = 0; j < l.count; ++j) {
            const int to = MOVE_TO(l.moves[j]), from = MOVE_FROM(l.moves[j]);
            int taken = p.piece[to] != NO_PIECE && p.color[to] == us ? p.piece[to] : NO_PIECE;
            if (taken == NO_PIECE && p.piece[from] == PAWN && to == p.ep) taken = PAWN;
            if (taken == NO_PIECE || taken == KING) continue;
            int gain = piece_pawns[taken];
            if (gain <= best) continue;
            Position q = p; Undo v; make_move(&q, l.moves[j], &v);
            if (is_attacked(&q, to, us)) gain -= piece_pawns[p.piece[from]];
            if (gain > best) best = gain;
        }
        lost += w * (best - got);   /* the net of the exchange this board sees */
    }
    return total > 0 ? lost / total : 0;
}

/* king_risk on at most `cap` boards of P, evenly strided (weights kept) */
static double king_risk_capped(const Belief *b, Move m, size_t cap) {
    if (b->count <= cap) return king_risk(b, m);
    const size_t stride = (b->count + cap - 1) / cap;
    double total = 0, lost = 0;
    for (size_t i = 0; i < b->count; i += stride) {
        const double w = b->logw ? exp((double)b->logw[i]) : 1.0;
        Position p = b->items[i];
        Undo u;
        make_move(&p, m, &u);
        total += w;
        if (mover_left_king_capturable(&p)) lost += w;
    }
    return total > 0 ? lost / total : 0;
}

static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

/* (remaining - reserve) / divisor + 80% of the increment, capped at max_seconds
 * and at 30% of what is left. The arena games are decided early: of 283
 * decisive bad moves in our v25-v33 losses, half came by our 10th decision and
 * 90% by the 18th, while remaining/30 left a median 77 s unused at the end.
 * MIST_DIVISOR and MIST_RESERVE set the shape (defaults 30 and 0 = v9). */
static double budget_divisor = 30.0, budget_reserve = 0.0;
static double move_budget(double remaining, double increment, double max_seconds) {
    double budget = (remaining - budget_reserve) / budget_divisor + 0.8 * increment;
    if (budget < 0.3) budget = 0.3;
    if (budget > max_seconds) budget = max_seconds;
    if (budget > remaining * 0.3) budget = remaining * 0.3;
    return budget;
}

int main(void) {
    char line[256];
    if (!fgets(line, sizeof line, stdin) || strncmp(line, "fow", 3) != 0) return 0;
    char side[4]; double base = 180, increment = 2;
    if (!fgets(line, sizeof line, stdin) || sscanf(line, "%3s %lf %lf", side, &base, &increment) < 1) return 0;
    const int us = side[0] == 'w' ? WHITE : BLACK;
    fow_init(); zobrist_init();
    const size_t cap = getenv("MIST_CAP") ? (size_t)strtoull(getenv("MIST_CAP"), NULL, 10) : 200000;
    const double max_seconds = getenv("MIST_MAX_SECONDS") ? atof(getenv("MIST_MAX_SECONDS")) : 10.0;
    if (getenv("MIST_DIVISOR")) budget_divisor = atof(getenv("MIST_DIVISOR"));
    if (getenv("MIST_RESERVE")) budget_reserve = atof(getenv("MIST_RESERVE"));
    const int repeat_lead = getenv("MIST_REPEAT_LEAD") ? atoi(getenv("MIST_REPEAT_LEAD")) : 2;
    /* a root move that leaves our king capturable counts kappa losses (gtcfr.h) */
    const double kappa = getenv("MIST_KING_RISK") ? atof(getenv("MIST_KING_RISK")) : 2.0;
    /* MIST_KING_GUARD: leave out moves this far above the safest king risk (negative: off) */
    const double guard = getenv("MIST_KING_GUARD") ? atof(getenv("MIST_KING_GUARD")) : 0.02;
    /* MIST_MATERIAL_GUARD: leave out moves whose expected one-ply material
     * loss on P exceeds the least by this many pawns (negative: off) */
    const double mguard = getenv("MIST_MATERIAL_GUARD") ? atof(getenv("MIST_MATERIAL_GUARD")) : -1.0;
    const size_t mcap = getenv("MIST_MATERIAL_CAP") ? (size_t)strtoull(getenv("MIST_MATERIAL_CAP"), NULL, 10) : 5000;
    /* decision replay (tests): our first decisions play these moves (comma
     * separated UCI, as the game went) and the next one is searched */
    const char *forced = getenv("MIST_FORCE_MOVES");
    /* MIST_STOCKFISH=path: Stockfish scores the search's leaves (paper, 3.3) */
    if (getenv("MIST_STOCKFISH") && *getenv("MIST_STOCKFISH"))
        fprintf(stderr, "mist: stockfish %s\n", sf_start(getenv("MIST_STOCKFISH")) ? "ready" : "failed to start; our evaluator scores the leaves");
    Belief b; belief_init_start(&b, us, cap);
    static Front front;
    front_init(&front, us);
    printf("ready\n"); fflush(stdout);

    char view[65] = "";
    int views = 0, moved = 0, decisions = 0, rebuilds = 0;
    Move last = 0;
    unsigned long long seed = getenv("FOW_SEED") ? strtoull(getenv("FOW_SEED"), NULL, 10) : 1;
    double clock_left = base;       /* our clock when the next view comes, from the last reading */
    double per_position = 2e-5;     /* seconds the opponent update costs per position of P, measured */
    double view_at = now_seconds(); /* our clock runs from the view before our turn */
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (is_view(line)) {
            memcpy(view, line, 65);
            view_at = now_seconds();
            if (views++ > 0 && b.count > 0) {
                if (moved) belief_after_own(&b, last, view);
                else {
                    const size_t afford = (size_t)(0.5 * move_budget(clock_left, increment, max_seconds) / per_position);
                    if (b.count > afford && b.count > 2000) {
                        const size_t was = b.count;
                        belief_shrink(&b, afford > 2000 ? afford : 2000);
                        fprintf(stderr, "mist: clock %.1f s: P thinned from %zu to %zu before the update\n", clock_left, was, b.count);
                    }
                    const size_t parents = b.count;
                    const double started = now_seconds();
                    belief_after_opponent(&b, view);
                    if (parents >= 1000) per_position = 0.5 * per_position + 0.5 * (now_seconds() - started) / (double)parents;
                }
            }
            front_see(&front, view);
            if (b.count == 0 && !moved && views > 1) {
                /* our turn and nothing left: rebuild from what we see */
                const size_t n = belief_resample(&b, view, front.their, front.castle, 2000, 60000);
                fprintf(stderr, "mist: belief empty, rebuilt %zu positions from the view (rebuild %d)\n", n, ++rebuilds);
            }
            moved = 0;
            continue;
        }
        char *end; const double reported = strtod(line, &end);
        if (end == line) continue;
        /* our turn; the clock ran while we updated P. A larger sample of P
         * when there is time for it */
        const double remaining = reported - (now_seconds() - view_at);
        const double budget = move_budget(remaining, increment, max_seconds);
        SearchParams sp = {budget >= 3.0 ? 256 : 128, 2000, seed + (unsigned long long)decisions, budget * 0.6, 0};
        if (remaining < 20) { sp.samples = 32; sp.iterations = 60; sp.seconds = 0.3; }
        GtcfrParams gp = {budget >= 3.0 ? 256 : 128, budget, 1500000, 32, 10, seed + (unsigned long long)decisions, NULL, 0, kappa};
        /* With Stockfish at about 1 ms a call, 256 worlds spread the search too
         * thin to find the opponent's punishing reply: at v32's 21 decisions
         * that lost 3+ points to a top-six team, 256 worlds repeated the losing
         * move 11-13 times, 32 worlds 7-9, Misty (32 worlds) 5-6 */
        if (sf_ready()) gp.samples = 32;
        if (getenv("MIST_SAMPLES")) gp.samples = atoi(getenv("MIST_SAMPLES"));
        if (remaining < 20) { gp.samples = 32; gp.seconds = 0.3; }
        MoveList allowed;
        view_moves(view, us, front.castle, &allowed);
        /* ahead: leave out the moves that would make our pieces stand as they
         * already have twice (the referee's threefold needs that) */
        Move repeats[256];
        const int lead = front_lead(&front, view);
        if (lead >= repeat_lead && allowed.count > 0) {
            const int n = front_repeats(&front, view, &allowed, repeats);
            if (n > 0 && n < allowed.count) {
                gp.avoid = repeats; gp.avoid_count = n;
                fprintf(stderr, "mist: ahead by %d; %d moves would repeat our pieces a third time, left out\n", lead, n);
            }
        }
        /* king guard: our opponent model gives a hidden check (a queen on
         * a4 against e8) a few percent, so the search trades it for a pawn;
         * at v8's three 9-12-ply losses to v7 that check had 3.5-6% and six
         * or more moves had none. Leave out the moves more than `guard`
         * above the safest one, measured on all of P (or 20,000 boards) */
        Move guarded[512];
        double mrisk[256], mleast = 1e9;
        int have_mrisk = 0;
        if ((mguard >= 0 || getenv("MIST_RISK") || getenv("MIST_MATERIAL_DIAG")) && b.count > 0 && allowed.count > 1 && !(forced && *forced)) {
            for (int i = 0; i < allowed.count; ++i) { mrisk[i] = material_risk(&b, allowed.moves[i], mcap); if (mrisk[i] < mleast) mleast = mrisk[i]; }
            have_mrisk = 1;
        }
        if (guard >= 0 && b.count > 0 && allowed.count > 1 && !(forced && *forced)) {
            double risk[256], least = 1;
            for (int i = 0; i < allowed.count; ++i) { risk[i] = king_risk_capped(&b, allowed.moves[i], 20000); if (risk[i] < least) least = risk[i]; }
            int n = gp.avoid_count;
            for (int i = 0; i < n; ++i) guarded[i] = gp.avoid[i];
            int dropped = 0, mdropped = 0;
            for (int i = 0; i < allowed.count; ++i) {
                if (risk[i] > least + guard) { guarded[n++] = allowed.moves[i]; ++dropped; }
                else if (mguard >= 0 && have_mrisk && mrisk[i] > mleast + mguard) { guarded[n++] = allowed.moves[i]; ++mdropped; }
            }
            if (dropped + mdropped > 0 && dropped + mdropped < allowed.count) {
                gp.avoid = guarded; gp.avoid_count = n;
                if (dropped > 0) fprintf(stderr, "mist: king guard left out %d of %d moves (safest %.3f)\n", dropped, allowed.count, least);
                if (mdropped > 0) fprintf(stderr, "mist: material guard left out %d of %d moves (least %.2f)\n", mdropped, allowed.count, mleast);
            }
        }
        SearchResult r; int ok = 0;
        memset(&r, 0, sizeof r);
        if (forced && *forced) {
            char want[8] = ""; int k = 0;
            while (forced[k] && forced[k] != ',' && k < 7) { want[k] = forced[k]; ++k; }
            forced += forced[k] == ',' ? k + 1 : k;
            for (int i = 0; i < allowed.count && !ok; ++i) { char u[6]; move_to_uci(allowed.moves[i], u); if (strcmp(u, want) == 0) { r.move = allowed.moves[i]; ok = 1; } }
            if (!ok && b.count > 0) {   /* e.g. en passant the view cannot prove */
                MoveList l; generate_moves(&b.items[0], &l);
                for (int i = 0; i < l.count && !ok; ++i) { char u[6]; move_to_uci(l.moves[i], u); if (strcmp(u, want) == 0) { r.move = l.moves[i]; ok = 1; } }
            }
            if (!ok) { fprintf(stderr, "mist: forced move %s not found\n", want); break; }
            if (getenv("MIST_RISK")) {   /* diagnostics: the played move's risk on P, and the least */
                double least = 1;
                for (int i = 0; i < allowed.count; ++i) { const double x = king_risk(&b, allowed.moves[i]); if (x < least) least = x; }
                fprintf(stderr, "mist: played %s risk %.4f least %.4f |P|=%zu%s\n", want, king_risk(&b, r.move), least, b.count, b.overflowed ? "(capped)" : "");
            }
            printf("move %s\n", want); fflush(stdout);
            clock_left = reported - (now_seconds() - view_at) + increment;
            front_play(&front, view, r.move);
            last = r.move; moved = 1; ++decisions;
            continue;
        }
        /* GT-CFR by default (MIST_SEARCH=twoply for the fixed two-ply search) */
        const int twoply = getenv("MIST_SEARCH") && strcmp(getenv("MIST_SEARCH"), "twoply") == 0;
        if (b.count > 0) ok = twoply ? search_decide(&b, &sp, &r) : gtcfr_decide(&b, &gp, &r);
        if (!ok) {
            /* last resort: decide on the position we can see */
            Position seen; front_view_position(view, us, front.castle, &seen);
            Belief one = {0}; one.items = &seen; one.count = 1; one.us = us;
            ok = search_decide(&one, &sp, &r);
            fprintf(stderr, "mist: no belief, decided on the visible position\n");
        }
        if (allowed.count > 0) {
            int found = 0;
            for (int i = 0; i < allowed.count; ++i) found |= allowed.moves[i] == r.move;
            if (ok && !found && !b.overflowed) found = legal_everywhere(&b, r.move);
            if (!ok || !found) {
                char bad[6]; move_to_uci(r.move, bad);
                r.move = front_best_allowed(&front, view, &allowed);
                fprintf(stderr, "mist: %s is not legal on every board the view allows; playing a move it allows\n", ok ? bad : "(none)");
                ok = 1;
            }
        }
        if (!ok) { fprintf(stderr, "mist: no legal move\n"); break; }
        char uci[6]; move_to_uci(r.move, uci);
        double chosen_mrisk = -1;
        if (have_mrisk) for (int i = 0; i < allowed.count; ++i) if (allowed.moves[i] == r.move) chosen_mrisk = mrisk[i];
        if (getenv("MIST_RISK")) {   /* diagnostics: each allowed move's king and material risk on P */
            for (int i = 0; i < allowed.count; ++i) {
                char u[6]; move_to_uci(allowed.moves[i], u);
                fprintf(stderr, "mist: risk %s %.4f mrisk %.2f%s\n", u, king_risk(&b, allowed.moves[i]), have_mrisk ? mrisk[i] : -1.0, allowed.moves[i] == r.move ? " (chosen)" : "");
            }
        }
        long sf_calls; double sf_seconds;
        sf_stats(&sf_calls, &sf_seconds);
        fprintf(stderr, "mist: n=%d |P|=%zu%s samples=%d moves=%d infosets=%d nodes=%d iters=%d p=%.2f v=%+.3f lead=%+d clock=%.1f sf=%ld/%.1fs mr=%.2f/%.2f move=%s\n",
                ++decisions, b.count, b.overflowed ? "(capped)" : "", r.samples, r.our_moves, r.opponent_infosets, r.nodes,
                r.iterations, r.probability, r.value, lead, remaining, sf_calls, sf_seconds, chosen_mrisk, have_mrisk ? mleast : -1.0, uci);
        printf("move %s\n", uci); fflush(stdout);
        clock_left = reported - (now_seconds() - view_at) + increment;
        front_play(&front, view, r.move);
        last = r.move; moved = 1;
        if (forced) break;   /* decision replay: one searched decision */
    }
    belief_free(&b);
    sf_stop();
    return 0;
}
