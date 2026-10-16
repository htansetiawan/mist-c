#define _POSIX_C_SOURCE 199309L  /* clock_gettime under -std=c11 */
/* mist-c: growing-tree search (GT-CFR). See gtcfr.h. */
#include "gtcfr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfr.h"
#include "eval.h"
#include "sfeval.h"

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
    int best_init;               /* new information sets start on their best child */
    int fresh_uniform;           /* a walk into an information set newer than the strategy picks uniformly */
    unsigned long long rng;
} Tree;

#define MAX_WORKERS_BATCH 64   /* picks per round at most; sf workers are at most 8 */

/* Fog hazard (v17, MIST_FOG_HAZARD scale, MIST_FOG_HAZARD_THEM for the
 * opponent's king; both 0 = off). Stockfish scores a leaf as if both sides saw
 * the board; in Fog of War a king with open lines into unseen squares is taken
 * far more often. Measured on 1,446 v33-v35 arena games against the top seven
 * (positions right after our move; king captured within 4 plies):
 *             0 lines  1      2      3+
 *   middle    1.8%     7.3%   17.3%  36.1%
 *   late      1.7%     4.3%   7.8%   12.0%   (opponent non-pawn material <= 13)
 * The table below is 2 x (rate - rate with no lines): the drop in expected
 * value on the [-1, 1] scale. Lines are counted by eval_fog (open rays from
 * the king whose first empty square is unseen, only for slider kinds the other
 * side still has). */
static const double fog_hazard[2][4] = {{0.0, 0.11, 0.31, 0.69}, {0.0, 0.05, 0.12, 0.21}};
static double fog_scale = -1, fog_scale_them = 0;
static double fog_penalty(const Position *q, int color) {
    int fog[2];
    eval_fog(q, color, fog);
    const int lines = fog[0] > 3 ? 3 : fog[0];
    if (!lines) return 0;
    const bb_t them = q->by_color[color ^ 1];
    const int heavy = 3 * __builtin_popcountll(them & (q->by_type[KNIGHT] | q->by_type[BISHOP]))
                    + 5 * __builtin_popcountll(them & q->by_type[ROOK]) + 9 * __builtin_popcountll(them & q->by_type[QUEEN]);
    return fog_hazard[heavy <= 13][lines];
}
/* v18: the king move itself. On the same 1,446 games, after a king move the
 * capture rate within four plies by fog lines is 2.2/12.8/27.5/46.5% in the
 * middlegame (other moves 0.7/3.1/10.4/24.5) and 3.9/8.3/14.0/17.0% late
 * (other moves 1.5/3.3/5.0/7.4): the move walks the king onto squares the
 * fog attacks. Our king moves 16.6 times per 100 against the top teams' 11.6
 * (late 13.2 against Greek Gift's 1.8). The table is 2 x (king-move rate -
 * other-move rate), added to the standing hazard above for a leaf reached by
 * our king move (MIST_KING_MOVE_HAZARD scale, default 1.0). */
static const double king_move_hazard[2][4] = {{0.03, 0.19, 0.34, 0.44}, {0.05, 0.10, 0.18, 0.19}};
static double king_move_scale = -1;
static double king_move_penalty(const Position *q, int color) {
    int fog[2];
    eval_fog(q, color, fog);
    const int lines = fog[0] > 3 ? 3 : fog[0];
    const bb_t them = q->by_color[color ^ 1];
    const int heavy = 3 * __builtin_popcountll(them & (q->by_type[KNIGHT] | q->by_type[BISHOP]))
                    + 5 * __builtin_popcountll(them & q->by_type[ROOK]) + 9 * __builtin_popcountll(them & q->by_type[QUEEN]);
    return king_move_hazard[heavy <= 13][lines];
}
static double fog_adjust(const Position *q, int us, double value) {
    if (fog_scale < 0) {
        king_move_scale = getenv("MIST_KING_MOVE_HAZARD") ? atof(getenv("MIST_KING_MOVE_HAZARD")) : 1.0;
        /* 0.5 by default (v17): on the regression suite vs v14, hidden-check
         * kind 60 -> 58 of 300 (fixed 3, broke 1), fatal kind 48 -> 46 of 99
         * (fixed 3, broke 1) with cp loss 113 -> 105, material cp 133 -> 129
         * (fatal 5 -> 4). At 1.0 the material kind worsens (cp 138). */
        fog_scale = getenv("MIST_FOG_HAZARD") ? atof(getenv("MIST_FOG_HAZARD")) : 0.5;
        fog_scale_them = getenv("MIST_FOG_HAZARD_THEM") ? atof(getenv("MIST_FOG_HAZARD_THEM")) : 0.0;
    }
    if (fog_scale > 0) value -= fog_scale * fog_penalty(q, us);
    if (fog_scale_them > 0) value += fog_scale_them * fog_penalty(q, us ^ 1);
    return value < -1 ? -1 : value > 1 ? 1 : value;
}

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
/* `sf`, `sf_cp`, `sf_found`: Stockfish's scores of the mover's moves, fetched
 * by the caller (sf_submit/sf_collect over the workers); sf = how many. */
static int build_children_scored(Tree *t, const Position *p, const uint64_t hist[2], int *kids, const MoveList *only,
                                 int sf, const int *sf_cp, const int *sf_found) {
    const int mover = p->side, other = mover ^ 1, us = t->us;
    const int me = mover == us ? 0 : 1;   /* index into hist for the mover */
    MoveList l; generate_moves(p, &l);
    MoveList own;
    if (only) { own = l; l = *only; }
    /* `sf` is how many moves Stockfish scored; a full MultiPV set ranks the
     * rest below its worst line */
    const int ranked = sf > 0 && sf >= sf_multipv();
    double floor_v = 0;
    if (ranked) {
        int worst = 0, seen = 0;
        for (int a = 0; a < l.count; ++a) if (sf_found[a] && (!seen || sf_cp[a] < worst)) { worst = sf_cp[a]; seen = 1; }
        floor_v = tanh(worst / 400.0);
    }
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
        double value;
        if (sf > 0 && sf_found[a]) {
            const double v = tanh(sf_cp[a] / 400.0);
            value = mover == us ? v : -v;
        } else if (ranked) {
            /* Stockfish scored its MultiPV best moves and left this one out:
             * it is no better than the worst it scored */
            value = mover == us ? floor_v : -floor_v;
        } else {
            value = evaluate(&q, us);
        }
        value = fog_adjust(&q, us, value);
        if (king_move_scale > 0 && p->piece[MOVE_FROM(l.moves[a])] == KING) {
            /* the mover's king walked: our walk is charged, theirs credited */
            const double h = king_move_scale * king_move_penalty(&q, mover);
            value = mover == us ? value - h : value + h;
            value = value < -1 ? -1 : value > 1 ? 1 : value;
        }
        kids[a] = game_node(&t->g, NODE_TERMINAL, 0, -1, value);
        remember_leaf(t, kids[a], &q, h);
    }
    return l.count;
}

/* Stockfish scores every child in one call (paper, C.5); our evaluator
 * covers a child it does not score and the case where it is off */
static int build_children(Tree *t, const Position *p, const uint64_t hist[2], int *kids, const MoveList *only) {
    int sf_cp[256], sf_found[256];
    MoveList l; generate_moves(p, &l);
    if (only) l = *only;
    const int sf = sf_ready() ? sf_children(p, &l, sf_cp, sf_found) : 0;
    return build_children_scored(t, p, hist, kids, only, sf, sf_cp, sf_found);
}

/* Turns leaf `index` into a decision node of its side to move. */
static int expand_scored(Tree *t, int index, int sf, const int *sf_cp, const int *sf_found) {
    Leaf leaf = t->leaves[index];
    int kids[256];
    const int count = build_children_scored(t, &leaf.pos, leaf.hist, kids, NULL, sf, sf_cp, sf_found);
    if (count == 0) return 0;
    const int player = leaf.pos.side == t->us ? 0 : 1;
    const int before = t->g.n_infosets;
    const int I = infoset_for(t, player, leaf.hist[player], count);
    if (t->g.n_infosets > before && t->best_init) {
        /* a new information set starts on its best child by the leaf values
         * (the highest for us, the lowest for the opponent) instead of
         * uniformly: a uniform opponent averages its blunders and hides the
         * reply that punishes our move until the solver finds it */
        int best = 0;
        for (int a = 1; a < count; ++a) {
            const double va = t->g.nodes[kids[a]].value, vb = t->g.nodes[kids[best]].value;
            if (player == 0 ? va > vb : va < vb) best = a;
        }
        game_start_on(&t->g, I, best);
    }
    game_expand_leaf(&t->g, leaf.node, player, I, kids, count);
    t->leaf_of[leaf.node] = -1;
    return 1;
}

static int expand(Tree *t, int index) {
    int sf_cp[256], sf_found[256];
    MoveList l; generate_moves(&t->leaves[index].pos, &l);
    const int sf = sf_ready() ? sf_children(&t->leaves[index].pos, &l, sf_cp, sf_found) : 0;
    return expand_scored(t, index, sf, sf_cp, sf_found);
}

/* Expands leaves `picked[0..n)`: their positions go to the Stockfish workers
 * in waves of sf_workers() (one request in flight per worker), so N workers
 * score N leaves in the time of one. Without Stockfish, one by one. */
static int expand_batch(Tree *t, const int *picked, int n) {
    int grown = 0;
    const int W = sf_workers();
    if (W <= 0) {
        for (int i = 0; i < n; ++i) grown += expand(t, picked[i]);
        return grown;
    }
    for (int i = 0; i < n; i += W) {
        int sent[MAX_WORKERS_BATCH];
        for (int j = 0; j < W && i + j < n; ++j) sent[j] = sf_submit(j, &t->leaves[picked[i + j]].pos);
        for (int j = 0; j < W && i + j < n; ++j) {
            int sf_cp[256], sf_found[256];
            MoveList l; generate_moves(&t->leaves[picked[i + j]].pos, &l);
            const int sf = sent[j] ? sf_collect(j, &l, sf_cp, sf_found) : 0;
            grown += expand_scored(t, picked[i + j], sf, sf_cp, sf_found);
        }
    }
    return grown;
}

static void ensure_visits(Tree *t) {
    if (t->g.n_slots <= t->cap_visits) return;
    const int old = t->cap_visits;
    t->cap_visits = t->g.n_slots * 2;
    t->visits = realloc(t->visits, t->cap_visits * sizeof(int));
    memset(t->visits + old, 0, (t->cap_visits - old) * sizeof(int));
}

/* One selection: walk down from the root to the frontier leaf reached and
 * return its index (-1: none), taking it off the frontier so that this
 * round's later walks cannot pick it again. */
/* `x_slots` is how many slots `x` covers: information sets created by this
 * round's earlier expansions lie beyond it and act as this round's frontier. */
static int pick_once(Tree *t, int explorer, const double *x, int x_slots) {
    int node = t->g.root;
    for (int depth = 0; depth < 64; ++depth) {
        const GameNode *n = &t->g.nodes[node];
        if (n->type == NODE_TERMINAL) {
            const int li = node < t->cap_leaf_of ? t->leaf_of[node] : -1;
            if (li >= 0) t->leaf_of[node] = -1;
            return li;
        }
        int pick = 0;
        if (n->type == NODE_CHANCE) {
            double r = (next_random(&t->rng) >> 11) * (1.0 / 9007199254740992.0), acc = 0;
            for (int i = 0; i < n->count; ++i) { acc += t->g.prob[n->first + i]; if (r <= acc) { pick = i; break; } }
        } else {
            const int o = t->g.is_offset[n->infoset];
            if (o + n->count > t->cap_visits) return -1;
            const double *xs = x + o;
            double fresh[256];
            if (o + n->count > x_slots) {
                /* an information set made since the strategy was read: its
                 * own current strategy (the best-child start, typically) */
                if (!t->fresh_uniform) return -1;
                pcfr_infoset_strategy(&t->g, n->infoset, fresh);
                xs = fresh;
            }
            if (n->player == explorer) {
                if (next_random(&t->rng) & 1) {
                    /* uniform over the support of the current strategy */
                    int support[256], s = 0;
                    for (int a = 0; a < n->count; ++a) if (xs[a] > 1e-6) support[s++] = a;
                    pick = s ? support[next_random(&t->rng) % (unsigned long long)s] : (int)(next_random(&t->rng) % (unsigned long long)n->count);
                } else {
                    /* PUCT: current strategy as the prior, visit bonus */
                    int total = 0; for (int a = 0; a < n->count; ++a) total += t->visits[o + a];
                    double best = -1;
                    for (int a = 0; a < n->count; ++a) {
                        const double score = xs[a] + sqrt((double)total + 1.0) / (1.0 + t->visits[o + a]);
                        if (score > best) { best = score; pick = a; }
                    }
                }
            } else {
                double r = (next_random(&t->rng) >> 11) * (1.0 / 9007199254740992.0), acc = 0;
                pick = n->count - 1;
                for (int a = 0; a < n->count; ++a) { acc += xs[a]; if (r <= acc) { pick = a; break; } }
            }
            t->visits[o + pick] += 1;
        }
        node = t->g.child[n->first + pick];
    }
    return -1;
}

int gtcfr_decide(const Belief *b, const GtcfrParams *params, SearchResult *out) {
    memset(out, 0, sizeof(*out));
    if (b->count == 0) return 0;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    Tree t; memset(&t, 0, sizeof(t));
    game_init(&t.g); t.us = b->us; t.rng = params->seed ^ 0x67746366ULL;
    t.root_loss = params->king_risk > 1.0 ? -params->king_risk : -1.0;
    t.best_init = !(getenv("MIST_BEST_INIT") && atoi(getenv("MIST_BEST_INIT")) == 0);
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
    /* expand every reply of the opponent once: the two-ply start. Not with
     * Stockfish: at a few milliseconds a call it would take K x |moves| calls;
     * its depth-1 search already answers each move's captures, and the tree
     * grows where the strategy goes */
    if (!sf_ready() || getenv("MIST_TWOPLY"))
        for (int i = 0, n = t.n_leaves; i < n && t.g.n_nodes < params->max_nodes; ++i)
            if (t.leaf_of[t.leaves[i].node] == i) expand(&t, i);
    game_grow(&t.g);
    ensure_visits(&t);
    double *x = malloc(sizeof(double) * (size_t)t.g.n_slots);
    int iteration = 0, explorer = 0, grown = 0;
    const int W = sf_workers();
    /* MIST_ASYNC=1: pipelined growth. Every worker keeps a leaf in flight,
     * the solver iterates while they think (MIST_ITERS iterations per
     * expansion, default 0.3 like the round loop's 10 per 32), and
     * whichever answers first is expanded and replaced. On two cores it
     * expands 15-25% more leaves in the same time; the regression suite
     * (check and fatal decisions, 2.5 s) showed no gain from that, so the
     * round loop stays the default until a bench says otherwise. */
    const int async = W > 0 && getenv("MIST_ASYNC") && atoi(getenv("MIST_ASYNC")) != 0;
    if (async) {
        const double iters_per = getenv("MIST_ITERS") ? atof(getenv("MIST_ITERS")) : 0.3;
        int inflight[8];
        for (int w = 0; w < 8; ++w) inflight[w] = -1;
        t.fresh_uniform = 1;
        pcfr_current(&t.g, x);
        int x_slots = t.g.n_slots, stale = 0, since_refresh = 0;
        double credit = iters_per;
        while (seconds_since(&t0) < params->seconds * 0.85) {
            /* the walks read a strategy a few iterations or expansions old:
             * reading it costs a pass over every slot, like an iteration */
            if (stale >= 4 || since_refresh >= 8) {
                x = realloc(x, sizeof(double) * (size_t)t.g.n_slots);
                pcfr_current(&t.g, x);
                x_slots = t.g.n_slots; stale = 0; since_refresh = 0;
            }
            int busy = 0;
            for (int w = 0; w < W; ++w) {
                if (inflight[w] >= 0) { ++busy; continue; }
                if (t.g.n_nodes >= params->max_nodes) continue;
                const int li = pick_once(&t, explorer, x, x_slots);
                explorer ^= 1;
                if (li < 0) break;
                if (sf_submit(w, &t.leaves[li].pos)) { inflight[w] = li; ++busy; }
                else {   /* a position Stockfish does not take: our evaluator, now */
                    grown += expand_scored(&t, li, 0, NULL, NULL);
                    game_grow(&t.g); ensure_visits(&t); ++since_refresh;
                }
            }
            int ready = sf_poll(0);
            if (!ready) {
                if (credit >= 1.0 || !busy) { pcfr_iteration(&t.g, ++iteration); credit -= 1.0; ++stale; }
                else ready = sf_poll(2);
                if (!ready) continue;
            }
            for (int w = 0; w < W; ++w) {
                if (!(ready & (1 << w)) || inflight[w] < 0) continue;
                const int li = inflight[w]; inflight[w] = -1;
                int sf_cp[256], sf_found[256];
                MoveList l; generate_moves(&t.leaves[li].pos, &l);
                const int sf = sf_collect(w, &l, sf_cp, sf_found);
                grown += expand_scored(&t, li, sf, sf_cp, sf_found);
                ++since_refresh;
                if (credit < 50.0) credit += iters_per;
            }
            game_grow(&t.g); ensure_visits(&t);
        }
        /* answers still in flight are paid for: take them in */
        for (int w = 0; w < W; ++w) {
            if (inflight[w] < 0) continue;
            const int li = inflight[w]; inflight[w] = -1;
            int sf_cp[256], sf_found[256];
            MoveList l; generate_moves(&t.leaves[li].pos, &l);
            const int sf = sf_collect(w, &l, sf_cp, sf_found);
            grown += expand_scored(&t, li, sf, sf_cp, sf_found);
        }
        game_grow(&t.g); ensure_visits(&t);
    }
    while (!async && seconds_since(&t0) < params->seconds * 0.85) {
        for (int i = 0; i < params->iterations_per_round; ++i) pcfr_iteration(&t.g, ++iteration);
        if (t.g.n_nodes >= params->max_nodes) continue;
        x = realloc(x, sizeof(double) * (size_t)t.g.n_slots);
        pcfr_current(&t.g, x);
        const int x_slots = t.g.n_slots;
        int picked[MAX_WORKERS_BATCH], np = 0;
        for (int e = 0; e < params->expansions_per_round && np < MAX_WORKERS_BATCH && t.g.n_nodes < params->max_nodes; ++e) {
            const int li = pick_once(&t, explorer, x, x_slots);
            if (li >= 0) picked[np++] = li;
            explorer ^= 1;
        }
        grown += expand_batch(&t, picked, np);
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
