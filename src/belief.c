/* mist-c: exact belief (milestone M2). See belief.h. */
#include "belief.h"

#include <stdlib.h>
#include <string.h>

static uint64_t z_piece[2][6][64], z_side, z_castle[64], z_ep[64];

static uint64_t splitmix(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void zobrist_init(void) {
    uint64_t s = 20261009;
    for (int c = 0; c < 2; ++c)
        for (int t = 0; t < 6; ++t)
            for (int sq = 0; sq < 64; ++sq) z_piece[c][t][sq] = splitmix(&s);
    z_side = splitmix(&s);
    for (int sq = 0; sq < 64; ++sq) { z_castle[sq] = splitmix(&s); z_ep[sq] = splitmix(&s); }
}

uint64_t position_key(const Position *p) {
    uint64_t k = p->side ? z_side : 0;
    for (int c = 0; c < 2; ++c)
        for (bb_t b = p->by_color[c]; b; b &= b - 1) {
            int sq = __builtin_ctzll(b);
            k ^= z_piece[c][(int)p->piece[sq]][sq];
        }
    for (bb_t b = p->castle_rooks; b; b &= b - 1) k ^= z_castle[__builtin_ctzll(b)];
    if (p->ep >= 0) k ^= z_ep[p->ep];
    return k | 1;  /* 0 marks an empty slot in the set */
}

uint64_t view_hash(const char view[65]) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < 64; ++i) { h ^= (unsigned char)view[i]; h *= 1099511628211ULL; }
    return h;
}

uint64_t history_mix(uint64_t h, uint64_t value) {
    uint64_t z = h + 0x9E3779B97F4A7C15ULL * (value | 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Entries are deduplicated by position only: keeping every history apart grew
 * P from 11.8k to 28.6k positions on the arena traces (38/40 games over a 100k
 * cap instead of 24/40). Each position keeps the first history that reached
 * it as its representative, so search still keys the opponent by history. */
static uint64_t entry_key(const Position *p, uint64_t hist) { (void)hist; return position_key(p); }

static void set_reset(Belief *b, size_t expected) {
    size_t size = 1024;
    while (size < 2 * expected + 16) size <<= 1;
    free(b->set);
    b->set = calloc(size, sizeof(uint64_t));
    b->set_size = size;
    b->set_count = 0;
}

static void set_place(uint64_t *set, size_t size, uint64_t key) {
    size_t mask = size - 1, i = (size_t)(key * 0x9E3779B97F4A7C15ULL) & mask;
    while (set[i]) i = (i + 1) & mask;
    set[i] = key;
}

/* Inserts `key`; returns 1 if it was new. The table doubles before it passes
 * half full: one hidden opponent move can leave twenty successors per position,
 * far more than any fixed guess, and a full open-addressing table never ends
 * its probe. */
static int set_insert(Belief *b, uint64_t key) {
    size_t mask = b->set_size - 1, i = (size_t)(key * 0x9E3779B97F4A7C15ULL) & mask;
    while (b->set[i]) {
        if (b->set[i] == key) return 0;
        i = (i + 1) & mask;
    }
    b->set[i] = key;
    if (++b->set_count * 2 > b->set_size) {
        size_t size = b->set_size * 2;
        uint64_t *grown = calloc(size, sizeof(uint64_t));
        for (size_t j = 0; j < b->set_size; ++j) if (b->set[j]) set_place(grown, size, b->set[j]);
        free(b->set);
        b->set = grown;
        b->set_size = size;
    }
    return 1;
}

void belief_init_start(Belief *b, int our_color, size_t cap) {
    memset(b, 0, sizeof(*b));
    b->us = our_color;
    b->cap = cap;
    b->rng = 0x6d6973742d63ULL;
    b->capacity = 1024;
    b->items = malloc(b->capacity * sizeof(Position));
    b->hist = malloc(b->capacity * sizeof(uint64_t));
    pos_start(&b->items[0]);
    char view[65];
    observation(&b->items[0], our_color ^ 1, view);
    b->hist[0] = history_mix(0, view_hash(view));   /* the opponent's start view */
    b->count = 1;
    set_reset(b, 1024);
    set_insert(b, entry_key(&b->items[0], b->hist[0]));
}

void belief_free(Belief *b) {
    free(b->items);
    free(b->hist);
    free(b->set);
    memset(b, 0, sizeof(*b));
}

/* Rebuilds P from candidate successors; `view` filters them. */
typedef struct { Position *items; uint64_t *hist; size_t count, capacity; } Vec;

static void vec_push(Vec *v, const Position *p, uint64_t hist) {
    if (v->count == v->capacity) {
        v->capacity = v->capacity ? v->capacity * 2 : 4096;
        v->items = realloc(v->items, v->capacity * sizeof(Position));
        v->hist = realloc(v->hist, v->capacity * sizeof(uint64_t));
    }
    v->items[v->count] = *p; v->hist[v->count] = hist; ++v->count;
}

static uint64_t next_random(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Keeps a uniform random sample of size cap among all consistent candidates
 * (reservoir sampling); `seen` counts the candidates offered so far. */
static void offer(Belief *b, Vec *next, size_t *seen, const Position *p, uint64_t hist) {
    ++*seen;
    if (next->count < b->cap) { vec_push(next, p, hist); return; }
    b->overflowed = 1;
    const uint64_t j = next_random(&b->rng) % (uint64_t)*seen;
    if (j < b->cap) { next->items[j] = *p; next->hist[j] = hist; }
}

static size_t commit(Belief *b, Vec *next) {
    free(b->items); free(b->hist);
    b->items = next->items;
    b->hist = next->hist;
    b->count = next->count;
    b->capacity = next->capacity;
    return b->count;
}

/* What a view pins down, as bitboards: necessary conditions checked before the
 * full visibility comparison. */
typedef struct { bb_t known, empty, ours[6], theirs[6]; } ViewMasks;

static void view_masks(const char view[65], int us, ViewMasks *m) {
    static const char *letters[2] = {"PNBRQK", "pnbrqk"};
    memset(m, 0, sizeof(*m));
    for (int sq = 0; sq < 64; ++sq) {
        char c = view[sq];
        if (c == '?') continue;
        m->known |= (bb_t)1 << sq;
        if (c == '-') { m->empty |= (bb_t)1 << sq; continue; }
        for (int t = 0; t < 6; ++t) {
            if (letters[us][t] == c) m->ours[t] |= (bb_t)1 << sq;
            if (letters[us ^ 1][t] == c) m->theirs[t] |= (bb_t)1 << sq;
        }
    }
}

static int may_match(const Position *p, const ViewMasks *m, int us) {
    bb_t occupied = p->by_color[0] | p->by_color[1];
    if (occupied & m->empty) return 0;
    for (int t = 0; t < 6; ++t) {
        if ((p->by_type[t] & p->by_color[us]) != m->ours[t]) return 0;
        if ((p->by_type[t] & p->by_color[us ^ 1] & m->known) != m->theirs[t]) return 0;
    }
    return 1;
}

size_t belief_after_opponent(Belief *b, const char view[65]) {
    Vec next = {0};
    set_reset(b, b->count * 4);
    char seen[65];
    ViewMasks vm;
    view_masks(view, b->us, &vm);
    size_t offered = 0;
    char theirs[65];
    for (size_t i = 0; i < b->count; ++i) {
        Position p = b->items[i];
        MoveList l;
        generate_moves(&p, &l);
        for (int j = 0; j < l.count; ++j) {
            Undo u;
            make_move(&p, l.moves[j], &u);
            if (!may_match(&p, &vm, b->us)) { unmake_move(&p, l.moves[j], &u); continue; }
            observation(&p, b->us, seen);
            if (memcmp(seen, view, 64) == 0) {
                /* the opponent knows its own move and sees its view after it */
                observation(&p, b->us ^ 1, theirs);
                const uint64_t h = history_mix(history_mix(b->hist[i], l.moves[j]), view_hash(theirs));
                if (set_insert(b, entry_key(&p, h))) offer(b, &next, &offered, &p, h);
            }
            unmake_move(&p, l.moves[j], &u);
        }
    }
    return commit(b, &next);
}

size_t belief_after_own(Belief *b, Move m, const char view[65]) {
    Vec next = {0};
    set_reset(b, b->count);
    char seen[65];
    ViewMasks vm;
    view_masks(view, b->us, &vm);
    char theirs[65];
    for (size_t i = 0; i < b->count; ++i) {
        Position p = b->items[i];
        Undo u;
        make_move(&p, m, &u);
        if (!may_match(&p, &vm, b->us)) continue;
        observation(&p, b->us, seen);
        if (memcmp(seen, view, 64) != 0) continue;
        /* the opponent does not see our move, only its view after it */
        observation(&p, b->us ^ 1, theirs);
        const uint64_t h = history_mix(b->hist[i], view_hash(theirs));
        if (set_insert(b, entry_key(&p, h))) vec_push(&next, &p, h);
    }
    return commit(b, &next);
}

int belief_contains(const Belief *b, const Position *p) {
    uint64_t key = position_key(p);
    for (size_t i = 0; i < b->count; ++i)
        if (position_key(&b->items[i]) == key) return 1;
    return 0;
}
