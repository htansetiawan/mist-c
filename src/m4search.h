/* Experimental chess adapter for the paper-only M4 core. Separate from the
 * active GT-CFR frontend so Fable can review/integrate without a file conflict.
 * Every view (including the view after our move) must be delivered in order.
 * Record the ACTUALLY played move, including a forced replay or safety fallback.
 */
#ifndef MIST_M4SEARCH_H
#define MIST_M4SEARCH_H
#include "search.h"
#include "m4sf.h"

typedef struct M4Search M4Search;
typedef struct {
    int samples, max_nodes, max_iterations;
    int expansions_per_round, iterations_per_round;
    double seconds; /* setup, solve and snapshot share this budget */
    uint64_t seed;
    int tactical_witnesses; /* opt-in root coverage experiment, not a posterior */
} M4Params;
typedef struct {
    int ply, frontier, current_found, current_positive, carried, off_current;
    int fresh, groups, expansions, snapshot, mode_changes, conservative_leaves;
    int fallback; /* 0 normal, 1 no saved I, 2 zero own reach, 3 prepare failure, 4 carry too large */
    int purified_support;
    int witness_scanned,witness_complete,witness_added,witness_candidate_moves;
    double setup_seconds, solve_seconds, snapshot_seconds, total_seconds;
    double active_mass, max_enter, current_mass;
    int over_budget;
    int child_alternates, sf_scored_moves;
    long sf_calls, sf_failed;
    double sf_seconds;
} M4Stats;

M4Search *m4_create(int us);
void m4_free(M4Search *s);
/* Borrow a separately owned optional evaluator. Child-backed fresh alternate
 * values may also be tested with the internal evaluator (sf=NULL). Neither
 * setting changes carried blueprint alternative values. */
void m4_set_evaluation(M4Search *s,M4SF *sf,int child_alternates);
int m4_observe(M4Search *s,const char view[65]);
int m4_played(M4Search *s,Move actual);
int m4_decide(M4Search *s,const Belief *belief,const M4Params *params,
               SearchResult *result,M4Stats *stats);
/* Read-only diagnostics, for protocol/history conformance tests. */
uint64_t m4_own_history(const M4Search *s);
int m4_ply(const M4Search *s);
#endif
