#define _POSIX_C_SOURCE 199309L  /* clock_gettime under -std=c11 */
/* mist-c: the two-ply belief subgame solved with PCFR+ (milestone M3). See search.h. */
#include "search.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfr.h"
#include "eval.h"

static const double kValue[6] = {1, 3, 3, 5, 9, 0};

double evaluate_material(const Position *p, int us) {
    double d = 0.0;
    for (int t = PAWN; t < KING; ++t)
        d += kValue[t] * (__builtin_popcountll(p->by_type[t] & p->by_color[us]) -
                          __builtin_popcountll(p->by_type[t] & p->by_color[us ^ 1]));
    return tanh(d / 6.0);
}

static unsigned long long next_random(unsigned long long *s) {
    unsigned long long z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

typedef struct { unsigned long long *keys; int *values; int size; } KeyMap;

static int keymap_get(KeyMap *m, unsigned long long key, int fresh) {
    unsigned long long i = key & (unsigned long long)(m->size - 1);
    while (m->keys[i]) {
        if (m->keys[i] == key) return m->values[i];
        i = (i + 1) & (unsigned long long)(m->size - 1);
    }
    m->keys[i] = key; m->values[i] = fresh;
    return -1;
}

int search_decide(const Belief *b, const SearchParams *params, SearchResult *out) {
    memset(out, 0, sizeof(*out));
    if (b->count == 0) return 0;
    const int us = b->us, them = us ^ 1;
    /* I: K distinct positions of P, uniformly (partial Fisher-Yates) */
    const int K = params->samples < (int)b->count ? params->samples : (int)b->count;
    int *idx = malloc(b->count * sizeof(int));
    for (size_t i = 0; i < b->count; ++i) idx[i] = (int)i;
    unsigned long long rng = params->seed ^ 0x5eedULL;
    for (int i = 0; i < K; ++i) {
        const int j = i + (int)(next_random(&rng) % (unsigned long long)(b->count - i));
        const int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }
    MoveList ours;
    generate_moves(&b->items[idx[0]], &ours);
    if (ours.count == 0) { free(idx); return 0; }

    Game g; game_init(&g);
    const int root_infoset = game_infoset(&g, 0, ours.count);
    const int root = game_node(&g, NODE_CHANCE, 0, -1, 0.0);
    int *chance_kids = malloc(K * sizeof(int)); double *chance_p = malloc(K * sizeof(double));
    KeyMap map = {0};
    map.size = 1; while (map.size < 4 * K * ours.count + 16) map.size <<= 1;
    map.keys = calloc(map.size, sizeof(unsigned long long)); map.values = malloc(map.size * sizeof(int));
    int *kids = malloc(256 * sizeof(int));
    char view[65];
    for (int k = 0; k < K; ++k) {
        Position p = b->items[idx[k]];
        const int our_node = game_node(&g, NODE_PLAYER, 0, root_infoset, 0.0);
        chance_kids[k] = our_node; chance_p[k] = 1.0 / K;
        int our_kids[256];
        for (int a = 0; a < ours.count; ++a) {
            Undo u; const Move m = ours.moves[a];
            make_move(&p, m, &u);
            if (king_square(&p, them) < 0) our_kids[a] = game_node(&g, NODE_TERMINAL, 0, -1, 1.0);
            else if (mover_left_king_capturable(&p)) our_kids[a] = game_node(&g, NODE_TERMINAL, 0, -1, -1.0);
            else {
                MoveList replies; generate_moves(&p, &replies);
                /* The opponent's information state: its own history in this
                 * position plus what it now sees. Our move is NOT part of it
                 * (Astra, note 035): it must play the same strategy across our
                 * root moves it cannot tell apart, and must not merge histories
                 * that merely share the current view. */
                observation(&p, them, view);
                /* a belief built by hand (the front end's visible-position
                 * fallback, tests) has no histories: treat them as empty */
                const uint64_t hist = b->hist ? b->hist[idx[k]] : 0;
                const unsigned long long key = history_mix(hist, view_hash(view)) | 1;
                int infoset = keymap_get(&map, key, g.n_infosets);
                if (infoset < 0) infoset = game_infoset(&g, 1, replies.count);
                /* identical opponent views imply identical opponent moves; if a
                 * capped belief ever breaks that, keep the node in its own infoset
                 * rather than corrupt the solver's action arrays */
                else if (g.is_actions[infoset] != replies.count) infoset = game_infoset(&g, 1, replies.count);
                const int opp = game_node(&g, NODE_PLAYER, 1, infoset, 0.0);
                for (int r = 0; r < replies.count; ++r) {
                    Undo v; make_move(&p, replies.moves[r], &v);
                    double leaf;
                    if (king_square(&p, us) < 0) leaf = -1.0;                 /* they took our king */
                    else if (mover_left_king_capturable(&p)) leaf = 1.0;      /* they left theirs to us */
                    else leaf = params->material_only ? evaluate_material(&p, us) : evaluate(&p, us);
                    kids[r] = game_node(&g, NODE_TERMINAL, 0, -1, leaf);
                    unmake_move(&p, replies.moves[r], &v);
                }
                if (replies.count == 0) {  /* no legal move: under the course rules the game is lost */
                    our_kids[a] = game_node(&g, NODE_TERMINAL, 0, -1, 1.0);
                } else {
                    game_children(&g, opp, kids, NULL, replies.count);
                    our_kids[a] = opp;
                }
            }
            unmake_move(&p, m, &u);
        }
        game_children(&g, our_node, our_kids, NULL, ours.count);
    }
    game_children(&g, root, chance_kids, chance_p, K);
    game_finalize(&g);
    /* PCFR+ until the iteration cap or the time budget, whichever first; at
     * least 10 iterations so the last iterate has moved off uniform. */
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    int t = 1;
    for (; t <= params->iterations; ++t) {
        pcfr_iteration(&g, t);
        if (params->seconds > 0 && t >= 10) {
            struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
            if ((t1.tv_sec - t0.tv_sec) + 1e-9 * (t1.tv_nsec - t0.tv_nsec) >= params->seconds) break;
        }
    }
    out->iterations = t > params->iterations ? params->iterations : t;
    double *x = malloc(g.n_slots * sizeof(double));
    pcfr_current(&g, x);
    const int o = g.is_offset[root_infoset];
    int best = 0;
    for (int a = 1; a < ours.count; ++a) if (x[o + a] > x[o + best]) best = a;
    out->move = ours.moves[best];
    out->probability = x[o + best];
    out->value = game_value(&g, x);
    out->samples = K; out->our_moves = ours.count; out->opponent_infosets = g.n_infosets - 1; out->nodes = g.n_nodes;
    free(x); free(kids); free(map.keys); free(map.values); free(chance_kids); free(chance_p); free(idx);
    game_free(&g);
    return 1;
}
