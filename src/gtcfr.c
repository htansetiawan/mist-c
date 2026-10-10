#define _POSIX_C_SOURCE 199309L  /* clock_gettime under -std=c11 */
/* mist-c: growing-tree search (GT-CFR). See gtcfr.h. */
#include "gtcfr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfr.h"
#include "eval.h"

typedef struct {
    int node;                    /* the Game node, currently an evaluated leaf */
    Position pos;                /* side to move = the next player */
    uint64_t hist[2];            /* information-state hashes: [our side, their side] */
} Leaf;

typedef struct {
    Game g;
    Leaf *leaves; int n_leaves, cap_leaves;
    int *leaf_of;                /* node -> index in leaves, or -1 */
    int cap_leaf_of;
    unsigned long long *keys; int *vals; int key_size, key_count;   /* infoset map */
    int *visits;                 /* per slot: expansions through (infoset, action) */
    int cap_visits;
    int us;
    double root_loss;            /* our root move leaving our king capturable: -kappa */
    unsigned long long rng;
} Tree;

static double seconds_since(const struct timespec *t0) {
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) + 1e-9 * (t1.tv_nsec - t0->tv_nsec);
}

static unsigned long long next_random(unsigned long long *s) {
    unsigned long long z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* infoset for (player, history), created with `actions` slots when new; a
 * mismatching action count (impossible for an exact view-history) gets its own */
static int infoset_for(Tree *t, int player, uint64_t hist, int actions) {
    if (t->key_count * 2 >= t->key_size) {
        int old = t->key_size; unsigned long long *ok = t->keys; int *ov = t->vals;
        t->key_size = old ? old * 2 : 1 << 16;
        t->keys = calloc(t->key_size, sizeof(unsigned long long)); t->vals = malloc(t->key_size * sizeof(int));
        for (int i = 0; i < old; ++i) if (ok[i]) {
            unsigned long long j = ok[i] & (unsigned long long)(t->key_size - 1);
            while (t->keys[j]) j = (j + 1) & (unsigned long long)(t->key_size - 1);
            t->keys[j] = ok[i]; t->vals[j] = ov[i];
        }
        free(ok); free(ov);
    }
    const unsigned long long key = history_mix(hist, 0x5eed0000ULL + (unsigned long long)player) | 1;
    unsigned long long j = key & (unsigned long long)(t->key_size - 1);
    while (t->keys[j]) {
        if (t->keys[j] == key) {
            const int I = t->vals[j];
            if (t->g.is_actions[I] == actions) return I;
            return game_infoset(&t->g, player, actions);
        }
        j = (j + 1) & (unsigned long long)(t->key_size - 1);
    }
    const int I = game_infoset(&t->g, player, actions);
    t->keys[j] = key; t->vals[j] = I; ++t->key_count;
    return I;
}

static void remember_leaf(Tree *t, int node, const Position *p, const uint64_t hist[2]) {
    if (t->n_leaves == t->cap_leaves) {
        t->cap_leaves = t->cap_leaves ? 2 * t->cap_leaves : 4096;
        t->leaves = realloc(t->leaves, t->cap_leaves * sizeof(Leaf));
    }
    while (node >= t->cap_leaf_of) {
        const int old = t->cap_leaf_of;
        t->cap_leaf_of = old ? 2 * old : 1 << 16;
        t->leaf_of = realloc(t->leaf_of, t->cap_leaf_of * sizeof(int));
        for (int i = old; i < t->cap_leaf_of; ++i) t->leaf_of[i] = -1;
    }
    Leaf *l = &t->leaves[t->n_leaves];
    l->node = node; l->pos = *p; l->hist[0] = hist[0]; l->hist[1] = hist[1];
    t->leaf_of[node] = t->n_leaves++;
}

static int has_move(const MoveList *l, Move m) {
    for (int i = 0; i < l->count; ++i) if (l->moves[i] == m) return 1;
    return 0;
}

/* Children of `p` for its side to move: terminal nodes or new evaluated leaves.
 * `only` (the root) fixes the moves and their order; a move it lists that is
 * not legal in `p` (an en passant that only some boards allow) keeps `p`'s
 * evaluation. Returns the number of moves (0 = no legal move). */
static int build_children(Tree *t, const Position *p, const uint64_t hist[2], int *kids, const MoveList *only) {
    const int mover = p->side, other = mover ^ 1, us = t->us;
    const int me = mover == us ? 0 : 1;   /* index into hist for the mover */
    MoveList l; generate_moves(p, &l);
    MoveList own;
    if (only) { own = l; l = *only; }
    char view[65];
    for (int a = 0; a < l.count; ++a) {
        if (only && !has_move(&own, l.moves[a])) { kids[a] = game_node(&t->g, NODE_TERMINAL, 0, -1, evaluate(p, us)); continue; }
        Position q = *p; Undo u; make_move(&q, l.moves[a], &u);
        if (king_square(&q, other) < 0) { kids[a] = game_node(&t->g, NODE_TERMINAL, 0, -1, mover == us ? 1.0 : -1.0); continue; }
        if (mover_left_king_capturable(&q)) { kids[a] = game_node(&t->g, NODE_TERMINAL, 0, -1, mover == us ? (only ? t->root_loss : -1.0) : 1.0); continue; }
        MoveList next; generate_moves(&q, &next);
        if (next.count == 0) { kids[a] = game_node(&t->g, NODE_TERMINAL, 0, -1, mover == us ? 1.0 : -1.0); continue; }
        uint64_t h[2];
        observation(&q, mover, view); h[me] = history_mix(history_mix(hist[me], l.moves[a]), view_hash(view));
        observation(&q, other, view); h[1 - me] = history_mix(hist[1 - me], view_hash(view));
        kids[a] = game_node(&t->g, NODE_TERMINAL, 0, -1, evaluate(&q, us));
        remember_leaf(t, kids[a], &q, h);
    }
    return l.count;
}

/* Turns leaf `index` into a decision node of its side to move. */
static int expand(Tree *t, int index) {
    Leaf leaf = t->leaves[index];
    int kids[256];
    const int count = build_children(t, &leaf.pos, leaf.hist, kids, NULL);
    if (count == 0) return 0;
    const int player = leaf.pos.side == t->us ? 0 : 1;
    const int I = infoset_for(t, player, leaf.hist[player], count);
    game_expand_leaf(&t->g, leaf.node, player, I, kids, count);
    t->leaf_of[leaf.node] = -1;
    return 1;
}

static void ensure_visits(Tree *t) {
    if (t->g.n_slots <= t->cap_visits) return;
    const int old = t->cap_visits;
    t->cap_visits = t->g.n_slots * 2;
    t->visits = realloc(t->visits, t->cap_visits * sizeof(int));
    memset(t->visits + old, 0, (t->cap_visits - old) * sizeof(int));
}

/* One expansion: walk down from the root and expand the frontier leaf reached. */
/* `x_slots` is how many slots `x` covers: information sets created by this
 * round's earlier expansions lie beyond it and act as this round's frontier. */
static int expand_once(Tree *t, int explorer, const double *x, int x_slots) {
    int node = t->g.root;
    for (int depth = 0; depth < 64; ++depth) {
        const GameNode *n = &t->g.nodes[node];
        if (n->type == NODE_TERMINAL) {
            const int li = node < t->cap_leaf_of ? t->leaf_of[node] : -1;
            return li >= 0 ? expand(t, li) : 0;
        }
        int pick = 0;
        if (n->type == NODE_CHANCE) {
            double r = (next_random(&t->rng) >> 11) * (1.0 / 9007199254740992.0), acc = 0;
            for (int i = 0; i < n->count; ++i) { acc += t->g.prob[n->first + i]; if (r <= acc) { pick = i; break; } }
        } else {
            const int o = t->g.is_offset[n->infoset];
            if (o + n->count > x_slots || o + n->count > t->cap_visits) return 0;
            if (n->player == explorer) {
                if (next_random(&t->rng) & 1) {
                    /* uniform over the support of the current strategy */
                    int support[256], s = 0;
                    for (int a = 0; a < n->count; ++a) if (x[o + a] > 1e-6) support[s++] = a;
                    pick = s ? support[next_random(&t->rng) % (unsigned long long)s] : (int)(next_random(&t->rng) % (unsigned long long)n->count);
                } else {
                    /* PUCT: current strategy as the prior, visit bonus */
                    int total = 0; for (int a = 0; a < n->count; ++a) total += t->visits[o + a];
                    double best = -1;
                    for (int a = 0; a < n->count; ++a) {
                        const double score = x[o + a] + sqrt((double)total + 1.0) / (1.0 + t->visits[o + a]);
                        if (score > best) { best = score; pick = a; }
                    }
                }
            } else {
                double r = (next_random(&t->rng) >> 11) * (1.0 / 9007199254740992.0), acc = 0;
                pick = n->count - 1;
                for (int a = 0; a < n->count; ++a) { acc += x[o + a]; if (r <= acc) { pick = a; break; } }
            }
            t->visits[o + pick] += 1;
        }
        node = t->g.child[n->first + pick];
    }
    return 0;
}

int gtcfr_decide(const Belief *b, const GtcfrParams *params, SearchResult *out) {
    memset(out, 0, sizeof(*out));
    if (b->count == 0) return 0;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    Tree t; memset(&t, 0, sizeof(t));
    game_init(&t.g); t.us = b->us; t.rng = params->seed ^ 0x67746366ULL;
    t.root_loss = params->king_risk > 1.0 ? -params->king_risk : -1.0;
    /* I: positions of P drawn by weight (belief_sample), each with its share */
    const int draws = params->samples > 0 ? params->samples : 1;
    int *idx = malloc(draws * sizeof(int));
    double *share = malloc(draws * sizeof(double));
    uint64_t draw_rng = params->seed ^ 0x5eedULL;
    const int K = belief_sample(b, draws, &draw_rng, idx, share);
    MoveList ours; generate_moves(&b->items[idx[0]], &ours);
    if (ours.count == 0) { free(idx); free(share); game_free(&t.g); return 0; }
    if (params->avoid_count > 0) {
        MoveList kept; kept.count = 0;
        for (int a = 0; a < ours.count; ++a) {
            int avoided = 0;
            for (int i = 0; i < params->avoid_count; ++i) avoided |= ours.moves[a] == params->avoid[i];
            if (!avoided) kept.moves[kept.count++] = ours.moves[a];
        }
        if (kept.count > 0) ours = kept;
    }
    /* root: chance over I -> our single information set (same view and history throughout P) */
    const int root = game_node(&t.g, NODE_CHANCE, 0, -1, 0.0);
    const uint64_t our_root_hist = 0x726f6f74ULL;
    const int root_I = infoset_for(&t, 0, our_root_hist, ours.count);
    int *chance_kids = malloc(K * sizeof(int)); double *chance_p = malloc(K * sizeof(double));
    int kids[256];
    for (int k = 0; k < K; ++k) {
        const Position *p = &b->items[idx[k]];
        const uint64_t hist[2] = {our_root_hist, b->hist ? b->hist[idx[k]] : 0};
        const int node = game_node(&t.g, NODE_TERMINAL, 0, -1, 0.0);
        const int count = build_children(&t, p, hist, kids, &ours);
        game_expand_leaf(&t.g, node, 0, root_I, kids, count);
        chance_kids[k] = node; chance_p[k] = share[k];
    }
    game_children(&t.g, root, chance_kids, chance_p, K);
    game_finalize(&t.g);
    /* expand every reply of the opponent once: the two-ply start */
    for (int i = 0, n = t.n_leaves; i < n && t.g.n_nodes < params->max_nodes; ++i)
        if (t.leaf_of[t.leaves[i].node] == i) expand(&t, i);
    game_grow(&t.g);
    ensure_visits(&t);
    double *x = malloc(sizeof(double) * (size_t)t.g.n_slots);
    int iteration = 0, explorer = 0, grown = 0;
    while (seconds_since(&t0) < params->seconds * 0.85) {
        for (int i = 0; i < params->iterations_per_round; ++i) pcfr_iteration(&t.g, ++iteration);
        if (t.g.n_nodes >= params->max_nodes) continue;
        x = realloc(x, sizeof(double) * (size_t)t.g.n_slots);
        pcfr_current(&t.g, x);
        const int x_slots = t.g.n_slots;
        for (int e = 0; e < params->expansions_per_round && t.g.n_nodes < params->max_nodes; ++e) {
            grown += expand_once(&t, explorer, x, x_slots);
            explorer ^= 1;
        }
        game_grow(&t.g);
        ensure_visits(&t);
    }
    for (int i = 0; i < 20; ++i) pcfr_iteration(&t.g, ++iteration);
    x = realloc(x, sizeof(double) * (size_t)t.g.n_slots);
    pcfr_current(&t.g, x);
    const int o = t.g.is_offset[root_I];
    int best = 0;
    for (int a = 1; a < ours.count; ++a) if (x[o + a] > x[o + best]) best = a;
    out->move = ours.moves[best];
    out->probability = x[o + best];
    out->value = game_value(&t.g, x);
    out->samples = K; out->our_moves = ours.count; out->opponent_infosets = t.g.n_infosets; out->nodes = t.g.n_nodes;
    out->iterations = iteration;
    free(x); free(idx); free(share); free(chance_kids); free(chance_p);
    free(t.leaves); free(t.leaf_of); free(t.keys); free(t.vals); free(t.visits);
    game_free(&t.g);
    (void)grown;
    return 1;
}
