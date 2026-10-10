/* mist-c: exact belief (milestone M2). See belief.h. */
#include "belief.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "movemodel.h"

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
 * MIST_MODEL_TEMP scales the model's log-likelihoods (default 1); the full
 * model (attack features) is used while |P| <= MIST_MODEL_FULL_BELOW (default
 * 10000), the cheap one above; MIST_PRUNE_CAPTURABLE=0 keeps positions the
 * referee's rule excludes. */
static int settings_read = 0, model_on = 1, prune_capturable = 1;
static float model_temp = 1.0f;
static size_t model_full_below = 10000;

static void read_settings(void) {
    if (settings_read) return;
    settings_read = 1;
    const char *e;
    if ((e = getenv("MIST_MODEL")) && e[0] == '0') model_on = 0;
    if ((e = getenv("MIST_MODEL_TEMP"))) model_temp = (float)atof(e);
    if ((e = getenv("MIST_MODEL_FULL_BELOW"))) model_full_below = (size_t)strtoull(e, NULL, 10);
    if ((e = getenv("MIST_PRUNE_CAPTURABLE")) && e[0] == '0') prune_capturable = 0;
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

/* A consistent successor found in the first pass: rebuilt in the second only
 * if it is kept. */
typedef struct { uint64_t key; uint32_t parent; Move move; float logw; } Candidate;

static int by_key(const void *a, const void *b) {
    const uint64_t x = ((const Candidate *)a)->key, y = ((const Candidate *)b)->key;
    return x < y ? -1 : x > y;
}

typedef struct { double key; uint32_t index; } Ranked;

static int by_rank(const void *a, const void *b) {
    const double x = ((const Ranked *)a)->key, y = ((const Ranked *)b)->key;
    return x < y ? -1 : x > y;
}

static float log_add(float a, float b) {
    if (a < b) { const float t = a; a = b; b = t; }
    return a + log1pf(expf(b - a));
}

size_t belief_after_opponent(Belief *b, const char view[65]) {
    read_settings();
    ViewMasks vm;
    view_masks(view, b->us, &vm);
    Candidate *cand = NULL;
    size_t n = 0, cap_cand = 0;
    char seen[65];
    const int full = b->count <= model_full_below;
    float logit[256];
    /* pass 1: every consistent successor, its key and log-weight */
    for (size_t i = 0; i < b->count; ++i) {
        Position p = b->items[i];
        MoveList l;
        generate_moves(&p, &l);
        float log_z = 0;
        if (model_on) {
            move_logits(&p, &l, b->last_own, full, logit);
            float top = -INFINITY;
            for (int j = 0; j < l.count; ++j) if (logit[j] > top) top = logit[j];
            double z = 0;
            for (int j = 0; j < l.count; ++j) z += exp((double)(logit[j] - top));
            log_z = top + (float)log(z);
        }
        const float base = b->logw ? b->logw[i] : 0.0f;
        for (int j = 0; j < l.count; ++j) {
            Undo u;
            make_move(&p, l.moves[j], &u);
            /* the referee ends the game on a move that leaves the mover's king capturable */
            if (may_match(&p, &vm, b->us) && !(prune_capturable && mover_left_king_capturable(&p))) {
                observation(&p, b->us, seen);
                if (memcmp(seen, view, 64) == 0) {
                    if (n == cap_cand) { cap_cand = cap_cand ? 2 * cap_cand : 1 << 14; cand = realloc(cand, cap_cand * sizeof(Candidate)); }
                    cand[n].key = position_key(&p);
                    cand[n].parent = (uint32_t)i;
                    cand[n].move = l.moves[j];
                    cand[n].logw = base + (model_on ? model_temp * (logit[j] - log_z) : 0.0f);
                    ++n;
                }
            }
            unmake_move(&p, l.moves[j], &u);
        }
    }
    /* positions reached by several histories: weights add; the heaviest
     * history represents the position (search keys the opponent by it) */
    if (n > 1) qsort(cand, n, sizeof(Candidate), by_key);
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
        qsort(r, m, sizeof(Ranked), by_rank);
        keep = b->cap;
        for (size_t i = 0; i < keep; ++i) chosen[i] = r[i].index;
        free(r);
    } else {
        for (size_t i = 0; i < m; ++i) chosen[i] = (uint32_t)i;
    }
    /* pass 2: rebuild the kept positions with their histories */
    Vec next = {0};
    char theirs[65];
    for (size_t k = 0; k < keep; ++k) {
        const Candidate *c = &cand[chosen[k]];
        Position p = b->items[c->parent];
        Undo u;
        make_move(&p, c->move, &u);
        /* the opponent knows its own move and sees its view after it */
        observation(&p, b->us ^ 1, theirs);
        const uint64_t h = history_mix(history_mix(b->hist ? b->hist[c->parent] : 0, c->move), view_hash(theirs));
        vec_push(&next, &p, h, c->logw);
    }
    free(chosen);
    free(cand);
    return commit(b, &next);
}

size_t belief_after_own(Belief *b, Move m, const char view[65]) {
    read_settings();
    b->last_own = m;
    Vec next = {0};
    char seen[65];
    ViewMasks vm;
    view_masks(view, b->us, &vm);
    char theirs[65];
    for (size_t i = 0; i < b->count; ++i) {
        Position p = b->items[i];
        Undo u;
        make_move(&p, m, &u);
        if (!may_match(&p, &vm, b->us)) continue;
        /* the referee ends the game on a move that leaves the mover's king
         * capturable: the game went on, so our move did not */
        if (prune_capturable && mover_left_king_capturable(&p)) continue;
        observation(&p, b->us, seen);
        if (memcmp(seen, view, 64) != 0) continue;
        /* the opponent does not see our move, only its view after it */
        observation(&p, b->us ^ 1, theirs);
        const uint64_t h = history_mix(b->hist ? b->hist[i] : 0, view_hash(theirs));
        vec_push(&next, &p, h, b->logw ? b->logw[i] : 0.0f);   /* distinct positions stay distinct */
    }
    return commit(b, &next);
}

static int by_index_u32(const void *a, const void *b) {
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
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
