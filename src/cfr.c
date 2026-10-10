/* mist-c: Predictive CFR+ on a generic game tree (milestone M3). See cfr.h. */
#include "cfr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void game_init(Game *g) { memset(g, 0, sizeof(*g)); g->root = -1; }

void game_free(Game *g) {
    free(g->nodes); free(g->child); free(g->prob);
    free(g->is_player); free(g->is_actions); free(g->is_offset);
    free(g->regret); free(g->last); free(g->strategy); free(g->strategy_sum);
    memset(g, 0, sizeof(*g));
}

int game_infoset(Game *g, int player, int actions) {
    if (g->n_infosets == g->cap_infosets) {
        g->cap_infosets = g->cap_infosets ? 2 * g->cap_infosets : 64;
        g->is_player = realloc(g->is_player, g->cap_infosets * sizeof(int));
        g->is_actions = realloc(g->is_actions, g->cap_infosets * sizeof(int));
        g->is_offset = realloc(g->is_offset, g->cap_infosets * sizeof(int));
    }
    g->is_player[g->n_infosets] = player;
    g->is_actions[g->n_infosets] = actions;
    g->is_offset[g->n_infosets] = g->n_slots;
    g->n_slots += actions;
    return g->n_infosets++;
}

int game_node(Game *g, int type, int player, int infoset, double value) {
    if (g->n_nodes == g->cap_nodes) {
        g->cap_nodes = g->cap_nodes ? 2 * g->cap_nodes : 256;
        g->nodes = realloc(g->nodes, g->cap_nodes * sizeof(GameNode));
    }
    GameNode *n = &g->nodes[g->n_nodes];
    n->type = type; n->player = player; n->infoset = infoset; n->first = 0; n->count = 0; n->value = value;
    if (g->root < 0) g->root = g->n_nodes;
    return g->n_nodes++;
}

void game_children(Game *g, int node, const int *children, const double *p, int count) {
    while (g->n_edges + count > g->cap_edges) {
        g->cap_edges = g->cap_edges ? 2 * g->cap_edges : 512;
        g->child = realloc(g->child, g->cap_edges * sizeof(int));
        g->prob = realloc(g->prob, g->cap_edges * sizeof(double));
    }
    g->nodes[node].first = g->n_edges; g->nodes[node].count = count;
    for (int i = 0; i < count; ++i) {
        g->child[g->n_edges + i] = children[i];
        g->prob[g->n_edges + i] = p ? p[i] : 0.0;
    }
    g->n_edges += count;
}

void game_finalize(Game *g) {
    g->regret = calloc(g->n_slots, sizeof(double));
    g->last = calloc(g->n_slots, sizeof(double));
    g->strategy = calloc(g->n_slots, sizeof(double));
    g->strategy_sum = calloc(g->n_slots, sizeof(double));
}

/* PRM+: x proportional to [R + m]^+, m the last instantaneous regret (the
 * prediction); uniform when nothing is positive. */
static void strategy_of(const Game *g, int I, double *x) {
    const int n = g->is_actions[I], o = g->is_offset[I];
    double total = 0.0;
    for (int a = 0; a < n; ++a) { const double v = g->regret[o + a] + g->last[o + a]; x[a] = v > 0 ? v : 0; total += x[a]; }
    for (int a = 0; a < n; ++a) x[a] = total > 0 ? x[a] / total : 1.0 / n;
}

void pcfr_current(const Game *g, double *out) {
    for (int I = 0; I < g->n_infosets; ++I) strategy_of(g, I, out + g->is_offset[I]);
}

void pcfr_average(const Game *g, double *out) {
    for (int I = 0; I < g->n_infosets; ++I) {
        const int n = g->is_actions[I], o = g->is_offset[I];
        double total = 0.0;
        for (int a = 0; a < n; ++a) total += g->strategy_sum[o + a];
        for (int a = 0; a < n; ++a) out[o + a] = total > 0 ? g->strategy_sum[o + a] / total : 1.0 / n;
    }
}

/* Counterfactual traversal for `player`: returns the node's value for
 * `player`; accumulates instantaneous regrets into `inst` weighted by the
 * opponents' (and chance's) reach `reach_others`. */
static double traverse(const Game *g, int node, int player, double reach_others, const double *x, double *inst) {
    const GameNode *n = &g->nodes[node];
    if (n->type == NODE_TERMINAL) return player == 0 ? n->value : -n->value;
    if (n->type == NODE_CHANCE) {
        double v = 0.0;
        for (int i = 0; i < n->count; ++i) {
            const double p = g->prob[n->first + i];
            if (p > 0) v += p * traverse(g, g->child[n->first + i], player, reach_others * p, x, inst);
        }
        return v;
    }
    const int I = n->infoset, o = g->is_offset[I];
    if (n->player != player) {
        double v = 0.0;
        for (int a = 0; a < n->count; ++a) {
            const double pa = x[o + a];
            if (pa > 0) v += pa * traverse(g, g->child[n->first + a], player, reach_others * pa, x, inst);
        }
        return v;
    }
    /* Fog of War positions can have over 100 moves: the stack buffer covers
     * small games, larger nodes allocate. */
    double stack_vals[64], v = 0.0;
    double *vals = n->count <= 64 ? stack_vals : malloc(n->count * sizeof(double));
    for (int a = 0; a < n->count; ++a) {
        vals[a] = traverse(g, g->child[n->first + a], player, reach_others, x, inst);
        v += x[o + a] * vals[a];
    }
    for (int a = 0; a < n->count; ++a) inst[o + a] += reach_others * (vals[a] - v);
    if (vals != stack_vals) free(vals);
    return v;
}

void pcfr_iteration(Game *g, int t) {
    double *x = malloc(g->n_slots * sizeof(double)), *inst = malloc(g->n_slots * sizeof(double));
    for (int player = 0; player < 2; ++player) {
        pcfr_current(g, x);
        memset(inst, 0, g->n_slots * sizeof(double));
        traverse(g, g->root, player, 1.0, x, inst);
        for (int I = 0; I < g->n_infosets; ++I) {
            if (g->is_player[I] != player) continue;
            const int n = g->is_actions[I], o = g->is_offset[I];
            for (int a = 0; a < n; ++a) {
                const double r = g->regret[o + a] + inst[o + a];
                g->regret[o + a] = r > 0 ? r : 0;     /* CFR+ clipping */
                g->last[o + a] = inst[o + a];          /* the prediction for the next iterate */
                g->strategy_sum[o + a] += (double)t * x[o + a];
            }
        }
    }
    free(x); free(inst);
}

static double value_of(const Game *g, int node, const double *x) {
    const GameNode *n = &g->nodes[node];
    if (n->type == NODE_TERMINAL) return n->value;
    double v = 0.0;
    for (int i = 0; i < n->count; ++i) {
        const double p = n->type == NODE_CHANCE ? g->prob[n->first + i] : x[g->is_offset[n->infoset] + i];
        if (p > 0) v += p * value_of(g, g->child[n->first + i], x);
    }
    return v;
}

double game_value(const Game *g, const double *strategy) { return value_of(g, g->root, strategy); }

/* Best response by repeated infoset-level improvement: the counterfactual
 * values of each of `player`'s actions are summed over its infoset, then each
 * infoset switches to its best action; with perfect recall this settles in at
 * most (depth of player's infosets) passes. */
double best_response_value(const Game *g, const double *strategy, int player) {
    double *x = malloc(g->n_slots * sizeof(double)), *inst = malloc(g->n_slots * sizeof(double));
    memcpy(x, strategy, g->n_slots * sizeof(double));
    for (int pass = 0; pass < 64; ++pass) {
        memset(inst, 0, g->n_slots * sizeof(double));
        traverse(g, g->root, player, 1.0, x, inst);
        int changed = 0;
        for (int I = 0; I < g->n_infosets; ++I) {
            if (g->is_player[I] != player) continue;
            const int n = g->is_actions[I], o = g->is_offset[I];
            /* inst holds reach-weighted (value(a) - value(x)); argmax over a is the best action */
            int best = 0;
            for (int a = 1; a < n; ++a) if (inst[o + a] > inst[o + best] + 1e-12) best = a;
            for (int a = 0; a < n; ++a) {
                const double want = a == best ? 1.0 : 0.0;
                if (fabs(x[o + a] - want) > 1e-12) changed = 1;
                x[o + a] = want;
            }
        }
        if (!changed) break;
    }
    const double v = game_value(g, x);
    free(x); free(inst);
    return player == 0 ? v : -v;
}

double exploitability(const Game *g, const double *strategy) {
    const double v = game_value(g, strategy);
    return (best_response_value(g, strategy, 0) - v) + (best_response_value(g, strategy, 1) + v);
}
