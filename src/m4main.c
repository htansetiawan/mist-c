#define _POSIX_C_SOURCE 199309L  /* clock_gettime under -std=c11 */
/* Experimental Astra M4 frontend, forked from main.c at ec17eb0.
 * Kept separate for review; build/mist and main.c remain unchanged.
 * This target defaults to M4 with the in-process evaluator.
 * MIST_SEARCH=gtcfr or twoply retain the existing comparison engines.
 * MIST_REPLAY_WARM=1 searches forced prefixes before recording actual moves.
 *
 * Original referee-protocol front end (milestone M5).
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
#include "m4search.h"
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

static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

/* about remaining/30 + 80% of the increment, capped at max_seconds and at 30%
 * of what is left */
static double move_budget(double remaining, double increment, double max_seconds) {
    double budget = remaining / 30.0 + 0.8 * increment;
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
    const int repeat_lead = getenv("MIST_REPEAT_LEAD") ? atoi(getenv("MIST_REPEAT_LEAD")) : 2;
    /* a root move that leaves our king capturable counts kappa losses (gtcfr.h) */
    const double kappa = getenv("MIST_KING_RISK") ? atof(getenv("MIST_KING_RISK")) : 2.0;
    const char *search_mode = getenv("MIST_SEARCH");
    const int use_m4 = !search_mode || strcmp(search_mode, "m4") == 0;
    const int replay_warm = getenv("MIST_REPLAY_WARM") && atoi(getenv("MIST_REPLAY_WARM")) != 0;
    const int m4_iterations = getenv("MIST_M4_ITERATIONS") ? atoi(getenv("MIST_M4_ITERATIONS")) : 0;
    /* decision replay (tests): our first decisions play these moves (comma
     * separated UCI, as the game went) and the next one is searched */
    const char *forced = getenv("MIST_FORCE_MOVES");
    /* Offline-only fixed parent limit removes machine-load-dependent thinning
     * from paired replay comparisons. Arena clock control stays unchanged. */
    size_t replay_parents = 0;
    const char *parents_env = getenv("MIST_REPLAY_PARENTS");
    if (forced && parents_env) {
        char *end; unsigned long long n = strtoull(parents_env, &end, 10);
        if (*end || n < 2000 || n > 2000000) { fprintf(stderr, "mist-m4: invalid MIST_REPLAY_PARENTS\n"); return 2; }
        replay_parents = (size_t)n;
    }
    /* MIST_STOCKFISH=path: Stockfish scores the search's leaves (paper, 3.3) */
    if (!use_m4 && getenv("MIST_STOCKFISH") && *getenv("MIST_STOCKFISH"))
        fprintf(stderr, "mist: stockfish %s\n", sf_start(getenv("MIST_STOCKFISH")) ? "ready" : "failed to start; our evaluator scores the leaves");
    M4Search *m4 = m4_create(us);
    if (!m4) return 2;
    M4SF *m4sf=NULL;
    if(use_m4&&getenv("MIST_STOCKFISH")&&*getenv("MIST_STOCKFISH"))m4sf=m4sf_open(getenv("MIST_STOCKFISH"),1.0);
    int child_alternates=m4sf!=NULL||(getenv("MIST_M4_ALT_CHILDREN")&&atoi(getenv("MIST_M4_ALT_CHILDREN"))!=0);
    m4_set_evaluation(m4,m4sf,child_alternates);
    if (use_m4) fprintf(stderr, "mist-m4: evaluator=%s child_alternates=%d kappa=1, fresh-world singleton knowledge; replay=%s\n", m4sf?"bounded-stockfish":"internal",child_alternates,replay_warm ? "searched-prefix" : "cold-prefix");
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
            if (!m4_observe(m4, line)) { fprintf(stderr, "mist-m4: invalid observation phase\n"); break; }
            memcpy(view, line, 65);
            view_at = now_seconds();
            if (views++ > 0 && b.count > 0) {
                if (moved) belief_after_own(&b, last, view);
                else {
                    const size_t afford = replay_parents ? replay_parents : (size_t)(0.5 * move_budget(clock_left, increment, max_seconds) / per_position);
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
        if (getenv("MIST_SAMPLES")) {
            char *end; long n = strtol(getenv("MIST_SAMPLES"), &end, 10);
            if (*end || n < 1 || n > 4096) { fprintf(stderr, "mist-m4: invalid MIST_SAMPLES\n"); break; }
            gp.samples = (int)n;
        }
        if (remaining < 20) { gp.samples = 32; gp.seconds = 0.3; }
        MoveList allowed;
        view_moves(view, us, front.castle, &allowed);
        /* ahead: leave out the moves that would make our pieces stand as they
         * already have twice (the referee's threefold needs that) */
        Move repeats[256];
        const int lead = front_lead(&front, view);
        if (!use_m4 && lead >= repeat_lead && allowed.count > 0) {
            const int n = front_repeats(&front, view, &allowed, repeats);
            if (n > 0 && n < allowed.count) {
                gp.avoid = repeats; gp.avoid_count = n;
                fprintf(stderr, "mist: ahead by %d; %d moves would repeat our pieces a third time, left out\n", lead, n);
            }
        }
        M4Params mp = {gp.samples, gp.max_nodes, m4_iterations, gp.expansions_per_round, gp.iterations_per_round, gp.seconds, gp.seed,
            getenv("MIST_M4_WITNESSES")&&atoi(getenv("MIST_M4_WITNESSES"))!=0};
        M4Stats ms; memset(&ms, 0, sizeof ms);
        SearchResult r; int ok = 0;
        memset(&r, 0, sizeof r);
        if (forced && *forced) {
            if (use_m4 && replay_warm) {
                SearchResult ignored;
                const int warmed = m4_decide(m4, &b, &mp, &ignored, &ms);
                fprintf(stderr, "mist-m4: prefix ok=%d ply=%d carry=%d offI=%d fresh=%d fallback=%d snapshot=%d seconds=%.4f\n", warmed, ms.ply, ms.carried, ms.off_current, ms.fresh, ms.fallback, ms.snapshot, ms.total_seconds);
                fprintf(stderr,"m4-prefix-eval: ply=%d ready=%d scored=%d calls=%ld failed=%ld seconds=%.6f\n",ms.ply,m4sf_ready(m4sf),ms.sf_scored_moves,ms.sf_calls,ms.sf_failed,ms.sf_seconds);
            }
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
            if (!m4_played(m4, r.move)) { fprintf(stderr, "mist-m4: invalid forced-move phase\n"); break; }
            last = r.move; moved = 1; ++decisions;
            continue;
        }
        /* M4 by default; retain explicit GT-CFR/twoply comparison modes. */
        const int twoply = getenv("MIST_SEARCH") && strcmp(getenv("MIST_SEARCH"), "twoply") == 0;
        if (b.count > 0) ok = use_m4 ? m4_decide(m4, &b, &mp, &r, &ms) : twoply ? search_decide(&b, &sp, &r) : gtcfr_decide(&b, &gp, &r);
        if (use_m4) fprintf(stderr, "mist-m4: ok=%d ply=%d found=%d positive=%d carry=%d offI=%d fresh=%d groups=%d expand=%d fallback=%d snapshot=%d setup=%.4f solve=%.4f save=%.4f total=%.4f over=%d active=%.5f enter=%.5f current=%.5f support=%d\n", ok, ms.ply, ms.current_found, ms.current_positive, ms.carried, ms.off_current, ms.fresh, ms.groups, ms.expansions, ms.fallback, ms.snapshot, ms.setup_seconds, ms.solve_seconds, ms.snapshot_seconds, ms.total_seconds, ms.over_budget, ms.active_mass, ms.max_enter, ms.current_mass, ms.purified_support);
        if(use_m4)fprintf(stderr,"m4-eval: child_alternates=%d ready=%d scored=%d calls=%ld failed=%ld seconds=%.6f\n",ms.child_alternates,m4sf_ready(m4sf),ms.sf_scored_moves,ms.sf_calls,ms.sf_failed,ms.sf_seconds);
        if(use_m4&&mp.tactical_witnesses)fprintf(stderr,"m4-witness: scanned=%d complete=%d added=%d candidate_moves=%d\n",ms.witness_scanned,ms.witness_complete,ms.witness_added,ms.witness_candidate_moves);
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
        /* Export observations-derived belief only AFTER choosing the target
         * move. Truth stays in the external replay checker, never this bot. */
        const char *dump_path = getenv("MIST_REPLAY_BELIEF_DUMP");
        if (forced && !*forced && dump_path) {
            FILE *dump = fopen(dump_path, "w");
            if (dump) {
                for (size_t i=0;i<b.count;++i) {
                    char board[65];pos_to_board_string(&b.items[i],board);
                    fprintf(dump,"%s %d %016llx %d %.9g\n",board,b.items[i].ep,
                        (unsigned long long)b.items[i].castle_rooks,b.items[i].halfmove,b.logw?b.logw[i]:0.0);
                }
                fclose(dump);
            } else fprintf(stderr,"mist-m4: belief dump unavailable\n");
        }
        if (getenv("MIST_RISK")) {   /* diagnostics: each allowed move's risk on P */
            for (int i = 0; i < allowed.count; ++i) {
                char u[6]; move_to_uci(allowed.moves[i], u);
                fprintf(stderr, "mist: risk %s %.4f%s\n", u, king_risk(&b, allowed.moves[i]), allowed.moves[i] == r.move ? " (chosen)" : "");
            }
        }
        long sf_calls; double sf_seconds;
        sf_stats(&sf_calls, &sf_seconds);
        fprintf(stderr, "mist: n=%d |P|=%zu%s samples=%d moves=%d infosets=%d nodes=%d iters=%d p=%.2f v=%+.3f lead=%+d clock=%.1f sf=%ld/%.1fs move=%s\n",
                ++decisions, b.count, b.overflowed ? "(capped)" : "", r.samples, r.our_moves, r.opponent_infosets, r.nodes,
                r.iterations, r.probability, r.value, lead, remaining, sf_calls, sf_seconds, uci);
        printf("move %s\n", uci); fflush(stdout);
        clock_left = reported - (now_seconds() - view_at) + increment;
        front_play(&front, view, r.move);
        if (!m4_played(m4, r.move)) { fprintf(stderr, "mist-m4: invalid move phase\n"); break; }
        last = r.move; moved = 1;
        if (forced) break;   /* decision replay: one searched decision */
    }
    m4_free(m4);
    m4sf_close(m4sf);
    belief_free(&b);
    sf_stop();
    return 0;
}
