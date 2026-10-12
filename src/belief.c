/* mist-c: exact belief (milestone M2). See belief.h. */
#include "belief.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "movemodel.h"
#include <pthread.h>

#define MAX_THREADS 8

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
    b->logw = malloc(b->capacity * sizeof(float));
    b->logw[0] = 0;
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
    free(b->logw);
    free(b->set);
    memset(b, 0, sizeof(*b));
}

/* The next P under construction. */
typedef struct { Position *items; uint64_t *hist; float *logw; size_t count, capacity; } Vec;

static void vec_push(Vec *v, const Position *p, uint64_t hist, float logw) {
    if (v->count == v->capacity) {
        v->capacity = v->capacity ? v->capacity * 2 : 4096;
        v->items = realloc(v->items, v->capacity * sizeof(Position));
        v->hist = realloc(v->hist, v->capacity * sizeof(uint64_t));
        v->logw = realloc(v->logw, v->capacity * sizeof(float));
    }
    v->items[v->count] = *p; v->hist[v->count] = hist; v->logw[v->count] = logw; ++v->count;
}

static uint64_t next_random(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static double unit_open(uint64_t *s) { return ((double)(next_random(s) >> 11) + 0.5) * (1.0 / 9007199254740992.0); }

/* Replaces P by `next`, log-weights shifted so the largest is 0. */
static size_t commit(Belief *b, Vec *next) {
    free(b->items); free(b->hist); free(b->logw);
    float top = -INFINITY;
    for (size_t i = 0; i < next->count; ++i) if (next->logw[i] > top) top = next->logw[i];
    for (size_t i = 0; i < next->count; ++i) next->logw[i] -= top;
    b->items = next->items;
    b->hist = next->hist;
    b->logw = next->logw;
    b->count = next->count;
    b->capacity = next->capacity;
    return b->count;
}

/* Settings, read once: MIST_MODEL=0 weighs every consistent position alike;
 * MIST_MODEL_TEMP scales the model's log-likelihoods (default 2: sharper than
 * the fitted model. On the regression suite at 2.5 s, 1.0 -> 2.0 cut the
 * hidden-check losses from 73 to 63 of 300 (fixed 16, broke 6) and the
 * centipawn loss on the material decisions by 7; 0.5 and 0.7 were worse on
 * every measure and 3.0 was inside the noise of 2.0. The true board's rank
 * barely moves: the gain is the sampled worlds concentrating on the model's
 * plausible attacker placements); the full model (attack features) is used
 * while |P| <= MIST_MODEL_FULL_BELOW (default 10000), the cheap one above;
 * MIST_PRUNE_CAPTURABLE=0 keeps positions the referee's rule excludes. */
static int settings_read = 0, model_on = 1, prune_capturable = 1;
/* MIST_THREADS: threads over P for the updates (default 2: the arena's cores);
 * below 4000 positions the update stays on one */
static int threads_setting = 2;
static float model_temp = 2.0f;
static size_t model_full_below = 10000;

/* Empirical opponent prior (MIST_HISTORY_TABLE, built by tools/history_table.c):
 * how often each move followed each information history in the arena, keyed
 * by the same hash the worlds carry in b->hist. Where the history was seen
 * MIST_HISTORY_MIN times or more (default 3), a hidden move's probability is
 * (count + n0 * model) / (N + n0) with n0 = MIST_HISTORY_N0 (default 4): the
 * bots repeat their openings, and the linear model alone gave 3...Qa5+ in the
 * Scandinavian 12% against the e-file checks' 88%, which cost four games. */
typedef struct { uint64_t hash; uint32_t move, count; } HistRec;
static HistRec *hist_table = NULL;
static size_t hist_n = 0;
static float hist_n0 = 4.0f;
static unsigned hist_min = 3;
static unsigned long hist_lookups = 0, hist_hits = 0;

static void read_history_table(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "mist: history table %s: cannot open\n", path); return; }
    char magic[8]; uint32_t version = 0, count = 0;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "MISTHIST", 8) != 0 || fread(&version, 4, 1, f) != 1 || version != 1 || fread(&count, 4, 1, f) != 1) {
        fprintf(stderr, "mist: history table %s: bad header\n", path); fclose(f); return;
    }
    hist_table = malloc((size_t)count * sizeof(HistRec));
    if (!hist_table || fread(hist_table, sizeof(HistRec), count, f) != (size_t)count) {
        fprintf(stderr, "mist: history table %s: short read\n", path); free(hist_table); hist_table = NULL; fclose(f); return;
    }
    fclose(f);
    hist_n = count;
    fprintf(stderr, "mist: history table %s: %zu records\n", path, hist_n);
}

/* Counts of the moves of `l` after history `h` into c[]; returns N(h). */
static unsigned history_counts(uint64_t h, const MoveList *l, unsigned *c) {
    memset(c, 0, sizeof(unsigned) * (size_t)l->count);
    size_t lo = 0, hi = hist_n;
    while (lo < hi) { const size_t mid = lo + (hi - lo) / 2; if (hist_table[mid].hash < h) lo = mid + 1; else hi = mid; }
    unsigned total = 0;
    for (size_t i = lo; i < hist_n && hist_table[i].hash == h; ++i) {
        total += hist_table[i].count;
        for (int j = 0; j < l->count; ++j) if ((uint32_t)l->moves[j] == hist_table[i].move) { c[j] = hist_table[i].count; break; }
    }
    return total;
}

void belief_history_stats(unsigned long *lookups, unsigned long *hits) { *lookups = hist_lookups; *hits = hist_hits; }

static void read_settings(void) {
    if (settings_read) return;
    settings_read = 1;
    const char *e;
    if ((e = getenv("MIST_MODEL")) && e[0] == '0') model_on = 0;
    if ((e = getenv("MIST_MODEL_TEMP"))) model_temp = (float)atof(e);
    if ((e = getenv("MIST_MODEL_FULL_BELOW"))) model_full_below = (size_t)strtoull(e, NULL, 10);
    if ((e = getenv("MIST_PRUNE_CAPTURABLE")) && e[0] == '0') prune_capturable = 0;
    if ((e = getenv("MIST_HISTORY_TABLE")) && *e) read_history_table(e);
    if ((e = getenv("MIST_HISTORY_N0"))) hist_n0 = (float)atof(e);
    if ((e = getenv("MIST_HISTORY_MIN"))) hist_min = (unsigned)atoi(e);
    if ((e = getenv("MIST_THREADS"))) threads_setting = atoi(e);
    if (threads_setting < 1) threads_setting = 1;
    if (threads_setting > MAX_THREADS) threads_setting = MAX_THREADS;
}

static int threads_for(size_t count) { return count >= 4000 ? threads_setting : 1; }

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

/* A consistent successor found in the first pass: rebuilt in the second only
 * if it is kept. */
typedef struct { uint64_t key; uint32_t parent; Move move; float logw; } Candidate;

static int by_key(const void *a, const void *b) {
    const uint64_t x = ((const Candidate *)a)->key, y = ((const Candidate *)b)->key;
    return x < y ? -1 : x > y;
}

typedef struct { double key; uint32_t index; } Ranked;

static int by_index_u32(const void *a, const void *b) {
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}


static int by_rank(const void *a, const void *b) {
    const double x = ((const Ranked *)a)->key, y = ((const Ranked *)b)->key;
    return x < y ? -1 : x > y;
}

static float log_add(float a, float b) {
    if (a < b) { const float t = a; a = b; b = t; }
    return a + log1pf(expf(b - a));
}

/* Pass 1 of the opponent update over the parents [lo, hi): every consistent
 * successor, its key and log-weight. One per thread; the tables it reads are
 * fixed for the game and everything it writes is its own. */
typedef struct {
    const Belief *b; const ViewMasks *vm; const char *view;
    size_t lo, hi;
    int full;
    Candidate *cand; size_t n, cap;
    unsigned long lookups, hits;
} OppPass1;

static void *opp_pass1(void *arg) {
    OppPass1 *w = arg;
    const Belief *b = w->b;
    char seen[65];
    float logit[256];
    for (size_t i = w->lo; i < w->hi; ++i) {
        Position p = b->items[i];
        MoveList l;
        generate_moves(&p, &l);
        float log_z = 0;
        unsigned hc[256], N = 0;
        if (model_on) {
            move_logits(&p, &l, b->last_own, w->full, logit);
            float top = -INFINITY;
            for (int j = 0; j < l.count; ++j) if (logit[j] > top) top = logit[j];
            double z = 0;
            for (int j = 0; j < l.count; ++j) z += exp((double)(logit[j] - top));
            log_z = top + (float)log(z);
            if (hist_table) {
                ++w->lookups;
                N = history_counts(b->hist ? b->hist[i] : 0, &l, hc);
                if (N < hist_min) N = 0; else ++w->hits;
            }
        }
        const float base = b->logw ? b->logw[i] : 0.0f;
        for (int j = 0; j < l.count; ++j) {
            Undo u;
            make_move(&p, l.moves[j], &u);
            /* the referee ends the game on a move that leaves the mover's king capturable */
            if (may_match(&p, w->vm, b->us) && !(prune_capturable && mover_left_king_capturable(&p))) {
                observation(&p, b->us, seen);
                if (memcmp(seen, w->view, 64) == 0) {
                    if (w->n == w->cap) { w->cap = w->cap ? 2 * w->cap : 1 << 14; w->cand = realloc(w->cand, w->cap * sizeof(Candidate)); }
                    Candidate *c = &w->cand[w->n++];
                    c->key = position_key(&p);
                    c->parent = (uint32_t)i;
                    c->move = l.moves[j];
                    c->logw = base + (!model_on ? 0.0f : N > 0
                        ? model_temp * logf(((float)hc[j] + hist_n0 * expf(logit[j] - log_z)) / ((float)N + hist_n0))
                        : model_temp * (logit[j] - log_z));
                }
            }
            unmake_move(&p, l.moves[j], &u);
        }
    }
    if (w->n > 1) qsort(w->cand, w->n, sizeof(Candidate), by_key);   /* the threads' runs merge in order */
    return NULL;
}

/* Moves the `keep` smallest keys of r[0..m) into r[0..keep), in no particular
 * order: a three-way partition (expected linear time), not a sort. */
static void select_smallest(Ranked *r, size_t m, size_t keep) {
    size_t lo = 0, hi = m;
    uint64_t seed = 0x5e1ec7ULL ^ m;
    while (hi - lo > 1 && keep > lo && keep < hi) {
        const size_t a = lo + (size_t)(next_random(&seed) % (hi - lo)), b = lo + (size_t)(next_random(&seed) % (hi - lo)), c = lo + (size_t)(next_random(&seed) % (hi - lo));
        const double ka = r[a].key, kb = r[b].key, kc = r[c].key;
        const double pivot = ka < kb ? (kb < kc ? kb : (ka < kc ? kc : ka)) : (ka < kc ? ka : (kb < kc ? kc : kb));
        size_t lt = lo, i = lo, gt = hi;
        while (i < gt) {
            if (r[i].key < pivot) { Ranked t = r[lt]; r[lt] = r[i]; r[i] = t; ++lt; ++i; }
            else if (r[i].key > pivot) { --gt; Ranked t = r[gt]; r[gt] = r[i]; r[i] = t; }
            else ++i;
        }
        /* [lo, lt) < pivot, [lt, gt) == pivot, [gt, hi) > pivot */
        if (keep <= lt) hi = lt;
        else if (keep >= gt) lo = gt;
        else return;   /* the boundary falls inside the equal band */
    }
}

/* Pass 2 over the kept candidates [lo, hi): the positions with their
 * histories, written straight into the next P at their index. */
typedef struct {
    const Belief *b; const Candidate *cand; const uint32_t *chosen;
    size_t lo, hi;
    Vec *next;
} OppPass2;

static void *opp_pass2(void *arg) {
    OppPass2 *w = arg;
    const Belief *b = w->b;
    char theirs[65];
    for (size_t k = w->lo; k < w->hi; ++k) {
        const Candidate *c = &w->cand[w->chosen[k]];
        Position p = b->items[c->parent];
        Undo u;
        make_move(&p, c->move, &u);
        /* the opponent knows its own move and sees its view after it */
        observation(&p, b->us ^ 1, theirs);
        w->next->items[k] = p;
        w->next->hist[k] = history_mix(history_mix(b->hist ? b->hist[c->parent] : 0, c->move), view_hash(theirs));
        w->next->logw[k] = c->logw;
    }
    return NULL;
}

size_t belief_after_opponent(Belief *b, const char view[65]) {
    read_settings();
    ViewMasks vm;
    view_masks(view, b->us, &vm);
    const int full = b->count <= model_full_below;
    const int T = threads_for(b->count);
    OppPass1 w1[MAX_THREADS];
    pthread_t tid[MAX_THREADS];
    for (int t = 0; t < T; ++t) {
        memset(&w1[t], 0, sizeof w1[t]);
        w1[t].b = b; w1[t].vm = &vm; w1[t].view = view; w1[t].full = full;
        w1[t].lo = b->count * (size_t)t / (size_t)T; w1[t].hi = b->count * (size_t)(t + 1) / (size_t)T;
    }
    int started = 1;
    for (int t = 1; t < T; ++t) { if (pthread_create(&tid[t], NULL, opp_pass1, &w1[t]) != 0) { w1[0].hi = w1[t].hi; break; } started = t + 1; }
    if (started < T) for (int t = started; t < T; ++t) w1[t].hi = w1[t].lo;   /* their parents fell to thread 0 */
    opp_pass1(&w1[0]);
    for (int t = 1; t < started; ++t) pthread_join(tid[t], NULL);
    size_t n = 0;
    for (int t = 0; t < T; ++t) { n += w1[t].n; hist_lookups += w1[t].lookups; hist_hits += w1[t].hits; }
    Candidate *cand = w1[0].cand;
    if (T > 1) {
        /* merge the threads' sorted runs */
        cand = malloc((n ? n : 1) * sizeof(Candidate));
        size_t at[MAX_THREADS];
        for (int t = 0; t < T; ++t) at[t] = 0;
        for (size_t k = 0; k < n; ++k) {
            int pick = -1;
            for (int t = 0; t < T; ++t)
                if (at[t] < w1[t].n && (pick < 0 || w1[t].cand[at[t]].key < w1[pick].cand[at[pick]].key)) pick = t;
            cand[k] = w1[pick].cand[at[pick]++];
        }
        for (int t = 0; t < T; ++t) free(w1[t].cand);
    }
    /* positions reached by several histories: weights add; the heaviest
     * history represents the position (search keys the opponent by it) */
    size_t m = 0;
    for (size_t i = 0; i < n; ) {
        size_t j = i + 1;
        Candidate best = cand[i];
        float total = cand[i].logw;
        for (; j < n && cand[j].key == cand[i].key; ++j) {
            total = log_add(total, cand[j].logw);
            if (cand[j].logw > best.logw) best = cand[j];
        }
        best.logw = total;
        cand[m++] = best;
        i = j;
    }
    /* past the cap: weighted sampling without replacement (exponential keys) */
    size_t keep = m;
    uint32_t *chosen = malloc((m ? m : 1) * sizeof(uint32_t));
    if (m > b->cap) {
        b->overflowed = 1;
        Ranked *r = malloc(m * sizeof(Ranked));
        for (size_t i = 0; i < m; ++i) { r[i].key = log(-log(unit_open(&b->rng))) - cand[i].logw; r[i].index = (uint32_t)i; }
        keep = b->cap;
        select_smallest(r, m, keep);
        for (size_t i = 0; i < keep; ++i) chosen[i] = r[i].index;
        free(r);
        qsort(chosen, keep, sizeof(uint32_t), by_index_u32);   /* in candidate order, whatever the partition did */
    } else {
        for (size_t i = 0; i < m; ++i) chosen[i] = (uint32_t)i;
    }
    /* pass 2: rebuild the kept positions with their histories */
    Vec next = {0};
    next.capacity = keep ? keep : 1;
    next.items = malloc(next.capacity * sizeof(Position));
    next.hist = malloc(next.capacity * sizeof(uint64_t));
    next.logw = malloc(next.capacity * sizeof(float));
    next.count = keep;
    const int T2 = threads_for(keep);
    OppPass2 w2[MAX_THREADS];
    for (int t = 0; t < T2; ++t) {
        w2[t].b = b; w2[t].cand = cand; w2[t].chosen = chosen; w2[t].next = &next;
        w2[t].lo = keep * (size_t)t / (size_t)T2; w2[t].hi = keep * (size_t)(t + 1) / (size_t)T2;
    }
    int started2 = 1;
    for (int t = 1; t < T2; ++t) { if (pthread_create(&tid[t], NULL, opp_pass2, &w2[t]) != 0) { w2[0].hi = w2[T2 - 1].hi; break; } started2 = t + 1; }
    opp_pass2(&w2[0]);
    for (int t = 1; t < started2; ++t) pthread_join(tid[t], NULL);
    free(chosen);
    free(cand);
    return commit(b, &next);
}

typedef struct {
    const Belief *b; const ViewMasks *vm; const char *view; Move m;
    size_t lo, hi;
    Vec next;
} OwnPass;

static void *own_pass(void *arg) {
    OwnPass *w = arg;
    const Belief *b = w->b;
    char seen[65], theirs[65];
    for (size_t i = w->lo; i < w->hi; ++i) {
        Position p = b->items[i];
        Undo u;
        make_move(&p, w->m, &u);
        if (!may_match(&p, w->vm, b->us)) continue;
        /* the referee ends the game on a move that leaves the mover's king
         * capturable: the game went on, so our move did not */
        if (prune_capturable && mover_left_king_capturable(&p)) continue;
        observation(&p, b->us, seen);
        if (memcmp(seen, w->view, 64) != 0) continue;
        /* the opponent does not see our move, only its view after it */
        observation(&p, b->us ^ 1, theirs);
        const uint64_t h = history_mix(b->hist ? b->hist[i] : 0, view_hash(theirs));
        vec_push(&w->next, &p, h, b->logw ? b->logw[i] : 0.0f);   /* distinct positions stay distinct */
    }
    return NULL;
}

size_t belief_after_own(Belief *b, Move m, const char view[65]) {
    read_settings();
    b->last_own = m;
    ViewMasks vm;
    view_masks(view, b->us, &vm);
    const int T = threads_for(b->count);
    OwnPass w[MAX_THREADS];
    pthread_t tid[MAX_THREADS];
    for (int t = 0; t < T; ++t) {
        memset(&w[t], 0, sizeof w[t]);
        w[t].b = b; w[t].vm = &vm; w[t].view = view; w[t].m = m;
        w[t].lo = b->count * (size_t)t / (size_t)T; w[t].hi = b->count * (size_t)(t + 1) / (size_t)T;
    }
    int started = 1;
    for (int t = 1; t < T; ++t) { if (pthread_create(&tid[t], NULL, own_pass, &w[t]) != 0) { w[0].hi = w[t].hi; break; } started = t + 1; }
    if (started < T) for (int t = started; t < T; ++t) w[t].hi = w[t].lo;
    own_pass(&w[0]);
    for (int t = 1; t < started; ++t) pthread_join(tid[t], NULL);
    Vec next = w[0].next;
    if (T > 1) {
        size_t n = 0;
        for (int t = 0; t < T; ++t) n += w[t].next.count;
        if (n > next.capacity) {
            next.capacity = n;
            next.items = realloc(next.items, n * sizeof(Position));
            next.hist = realloc(next.hist, n * sizeof(uint64_t));
            next.logw = realloc(next.logw, n * sizeof(float));
        }
        for (int t = 1; t < T; ++t) {
            const Vec *v = &w[t].next;
            if (v->count) {
                memcpy(next.items + next.count, v->items, v->count * sizeof(Position));
                memcpy(next.hist + next.count, v->hist, v->count * sizeof(uint64_t));
                memcpy(next.logw + next.count, v->logw, v->count * sizeof(float));
                next.count += v->count;
            }
            free(v->items); free(v->hist); free(v->logw);
        }
    }
    return commit(b, &next);
}


size_t belief_shrink(Belief *b, size_t target) {
    if (b->count <= target) return b->count;
    Ranked *r = malloc(b->count * sizeof(Ranked));
    for (size_t i = 0; i < b->count; ++i) {
        r[i].key = log(-log(unit_open(&b->rng))) - (b->logw ? b->logw[i] : 0.0f);
        r[i].index = (uint32_t)i;
    }
    qsort(r, b->count, sizeof(Ranked), by_rank);
    uint32_t *keep = malloc(target * sizeof(uint32_t));
    for (size_t k = 0; k < target; ++k) keep[k] = r[k].index;
    free(r);
    /* in their old order, so compacting in place never overwrites a kept one */
    qsort(keep, target, sizeof(uint32_t), by_index_u32);
    for (size_t k = 0; k < target; ++k) {
        b->items[k] = b->items[keep[k]];
        if (b->hist) b->hist[k] = b->hist[keep[k]];
        if (b->logw) b->logw[k] = b->logw[keep[k]];
    }
    free(keep);
    b->count = target;
    b->overflowed = 1;
    return target;
}

int belief_contains(const Belief *b, const Position *p) {
    uint64_t key = position_key(p);
    for (size_t i = 0; i < b->count; ++i)
        if (position_key(&b->items[i]) == key) return 1;
    return 0;
}

double belief_share(const Belief *b, const Position *p) {
    const uint64_t key = position_key(p);
    double total = 0, mine = 0;
    for (size_t i = 0; i < b->count; ++i) {
        const double w = b->logw ? exp((double)b->logw[i]) : 1.0;
        total += w;
        if (position_key(&b->items[i]) == key) mine += w;
    }
    return total > 0 ? mine / total : 0;
}

static int by_index(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int belief_sample(const Belief *b, int K, uint64_t *rng, int *idx, double *prob) {
    if (b->count == 0 || K <= 0) return 0;
    if (b->count <= (size_t)K) {
        double total = 0;
        for (size_t i = 0; i < b->count; ++i) { idx[i] = (int)i; prob[i] = b->logw ? exp((double)b->logw[i]) : 1.0; total += prob[i]; }
        for (size_t i = 0; i < b->count; ++i) prob[i] /= total;
        return (int)b->count;
    }
    /* K draws with replacement by weight: cumulative weights, binary search */
    double *cum = malloc(b->count * sizeof(double));
    double total = 0;
    for (size_t i = 0; i < b->count; ++i) { total += b->logw ? exp((double)b->logw[i]) : 1.0; cum[i] = total; }
    for (int k = 0; k < K; ++k) {
        const double x = unit_open(rng) * total;
        size_t lo = 0, hi = b->count - 1;
        while (lo < hi) { const size_t mid = (lo + hi) / 2; if (cum[mid] < x) lo = mid + 1; else hi = mid; }
        idx[k] = (int)lo;
    }
    free(cum);
    qsort(idx, (size_t)K, sizeof(int), by_index);
    int distinct = 0;
    for (int k = 0; k < K; ) {
        int j = k + 1;
        while (j < K && idx[j] == idx[k]) ++j;
        idx[distinct] = idx[k];
        prob[distinct] = (double)(j - k) / K;
        ++distinct;
        k = j;
    }
    return distinct;
}

/* --- when the capped belief has lost every position ------------------------ */

static double unit_random(uint64_t *s) { return (double)(next_random(s) >> 11) * (1.0 / 9007199254740992.0); }

/* How plausible an unseen square is for a hidden piece: pawns mostly stand
 * near their start rank, the king near its back rank. */
static double placement_weight(int type, int them, int sq) {
    const int rank = sq / 8;
    if (type == PAWN) {
        if (rank == 0 || rank == 7) return 0;
        const int advance = them == WHITE ? rank - 1 : 6 - rank;
        double w = 1;
        for (int i = 0; i < advance; ++i) w *= 0.6;
        return w;
    }
    if (type == KING) return rank == (them == WHITE ? 0 : 7) ? 3 : 1;
    return 1;
}

static int pick_square(bb_t free, int type, int them, uint64_t *rng) {
    double total = 0, w[64];
    for (int sq = 0; sq < 64; ++sq) { w[sq] = (free >> sq) & 1 ? placement_weight(type, them, sq) : 0; total += w[sq]; }
    if (total <= 0) return -1;
    double x = unit_random(rng) * total;
    for (int sq = 0; sq < 64; ++sq) { if (w[sq] <= 0) continue; if ((x -= w[sq]) < 0) return sq; }
    for (int sq = 63; sq >= 0; --sq) if (w[sq] > 0) return sq;
    return -1;
}

size_t belief_resample(Belief *b, const char view[65], const int their[6], bb_t our_castle, size_t target, size_t tries) {
    static const char *letters[2] = {"PNBRQK", "pnbrqk"};
    const int us = b->us, them = us ^ 1;
    char board[65];
    int visible[6] = {0};
    bb_t unseen = 0;
    for (int sq = 0; sq < 64; ++sq) {
        const char c = view[sq];
        board[sq] = c == '?' ? '-' : c;
        if (c == '?') unseen |= (bb_t)1 << sq;
        for (int t = 0; t < 6; ++t) if (letters[them][t] == c) ++visible[t];
    }
    board[64] = '\0';
    Position base;
    if (pos_from_board_string(&base, board, us, our_castle, -1) != 0) return b->count;
    /* The unseen pieces: what the opponent still has minus what we see; a
     * piece seen beyond its count was a promoted pawn. */
    int hidden[6];
    for (int t = 0; t < 6; ++t) hidden[t] = their[t] - visible[t];
    for (int t = KNIGHT; t <= QUEEN; ++t) if (hidden[t] < 0) { hidden[PAWN] += hidden[t]; hidden[t] = 0; }
    if (hidden[PAWN] < 0) hidden[PAWN] = 0;
    hidden[KING] = visible[KING] ? 0 : 1;
    /* Pawn rules pin some unseen squares. In front of our pawn there is a
     * piece (an empty square there is a legal push, hence seen); a pawn's
     * capture square is empty (a piece there could be taken, hence seen). */
    bb_t must_fill = 0, must_empty = 0;
    const int dr = us == WHITE ? 8 : -8, start = us == WHITE ? 1 : 6;
    for (bb_t p = base.by_type[PAWN] & base.by_color[us]; p; p &= p - 1) {
        const int sq = __builtin_ctzll(p), f = sq % 8, one = sq + dr;
        if (one < 0 || one >= 64) continue;
        if (view[one] == '?') must_fill |= (bb_t)1 << one;
        else if (view[one] == '-' && sq / 8 == start && view[one + dr] == '?') must_fill |= (bb_t)1 << (one + dr);
        if (f > 0 && view[one - 1] == '?') must_empty |= (bb_t)1 << (one - 1);
        if (f < 7 && view[one + 1] == '?') must_empty |= (bb_t)1 << (one + 1);
    }
    must_fill &= ~must_empty;
    Vec next = {0};
    set_reset(b, target);
    char seen[65], theirs[65];
    static const int order[6] = {KING, QUEEN, ROOK, BISHOP, KNIGHT, PAWN};
    for (size_t attempt = 0; attempt < tries && next.count < target; ++attempt) {
        Position p = base;
        int left[6];
        memcpy(left, hidden, sizeof left);
        bb_t open = unseen & ~must_empty;
        /* the pinned squares first, each with a piece that may stand there
         * (an extra pawn or knight if the count has run out) */
        for (bb_t m = must_fill; m; m &= m - 1) {
            const int sq = __builtin_ctzll(m);
            double total = 0, w[6];
            for (int t = 0; t < 6; ++t) { w[t] = left[t] * placement_weight(t, them, sq); total += w[t]; }
            int type = -1;
            if (total > 0) {
                double x = unit_random(&b->rng) * total;
                for (int t = 0; t < 6 && type < 0; ++t) if (w[t] > 0 && (x -= w[t]) < 0) type = t;
                if (type < 0) for (int t = 5; t >= 0 && type < 0; --t) if (w[t] > 0) type = t;
                --left[type];
            } else {
                type = placement_weight(PAWN, them, sq) > 0 ? PAWN : KNIGHT;
            }
            pos_put(&p, sq, them, type);
            open &= ~((bb_t)1 << sq);
        }
        for (int k = 0; k < 6; ++k) {
            const int t = order[k];
            for (; left[t] > 0; --left[t]) {
                const int sq = pick_square(open, t, them, &b->rng);
                if (sq < 0) break;   /* no room left: the piece is dropped */
                pos_put(&p, sq, them, t);
                open &= ~((bb_t)1 << sq);
            }
        }
        observation(&p, us, seen);
        if (memcmp(seen, view, 64) != 0) continue;
        if (!set_insert(b, position_key(&p))) continue;
        observation(&p, them, theirs);
        vec_push(&next, &p, history_mix(0x7265, view_hash(theirs)), 0.0f);
    }
    if (next.count == 0) { free(next.items); free(next.hist); free(next.logw); return b->count; }
    b->overflowed = 1;
    return commit(b, &next);
}

int view_moves(const char view[65], int us, bb_t our_castle, MoveList *out) {
    char board[65];
    for (int sq = 0; sq < 64; ++sq) board[sq] = view[sq] == '?' ? '-' : view[sq];
    board[64] = '\0';
    Position p;
    out->count = 0;
    if (pos_from_board_string(&p, board, us, our_castle, -1) != 0) return 0;
    MoveList all;
    generate_moves(&p, &all);
    for (int i = 0; i < all.count; ++i) {
        const Move m = all.moves[i];
        const int from = MOVE_FROM(m), to = MOVE_TO(m);
        if (view[to] == '?') continue;
        int lo = from < to ? from : to, hi = from < to ? to : from, ok = 1;
        if (p.piece[from] == PAWN && hi - lo == 16 && view[lo + 8] == '?') ok = 0;
        if (p.piece[from] == KING && hi - lo == 2) {
            /* castling: every square between king and rook */
            const int home = us == WHITE ? 0 : 56;
            const int a = to > from ? home + 5 : home + 1, z = to > from ? home + 6 : home + 3;
            for (int sq = a; sq <= z; ++sq) if (view[sq] == '?') ok = 0;
        }
        if (ok) out->moves[out->count++] = m;
    }
    /* En passant: an enemy pawn beside ours that we see although none of our
     * pieces attacks its square can only be seen as the pawn an en passant
     * capture takes, so that capture is legal. */
    const int dr = us == WHITE ? 8 : -8, fifth = us == WHITE ? 4 : 3;
    const char enemy_pawn = us == WHITE ? 'p' : 'P';
    for (bb_t b = p.by_type[PAWN] & p.by_color[us]; b; b &= b - 1) {
        const int from = __builtin_ctzll(b), f = from % 8;
        if (from / 8 != fifth) continue;
        for (int df = -1; df <= 1; df += 2) {
            if (f + df < 0 || f + df > 7) continue;
            const int beside = from + df, to = beside + dr;
            if (view[beside] == enemy_pawn && view[to] == '-' && !is_attacked(&p, beside, us))
                out->moves[out->count++] = MAKE_MOVE(from, to, NO_PIECE);
        }
    }
    return out->count;
}
