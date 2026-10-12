#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L  /* fork, pipe, poll, kill, clock_gettime under -std=c11 */
/* mist-c: Stockfish over UCI, on one or more worker processes. See sfeval.h. */
#include "sfeval.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_WORKERS 8

/* One Stockfish process with its pipes and a read buffer; at most one request
 * in flight (UCI wants the GUI to wait for bestmove before the next go). */
typedef struct {
    int to_fd, from_fd, alive, pending;
    pid_t child;
    char buf[1 << 16];
    size_t have, start;
    double started;
} Worker;

static Worker workers[MAX_WORKERS];
static int n_workers = 0, restarts = 0;
static char engine_path[512] = "";
static long calls = 0;
static double spent = 0;

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

static void stop_worker(Worker *w) {
    if (w->child > 0) {
        if (w->alive && w->to_fd >= 0) { const char q[] = "quit\n"; ssize_t r = write(w->to_fd, q, sizeof q - 1); (void)r; }
        kill(w->child, SIGKILL);
        waitpid(w->child, NULL, 0);
    }
    if (w->to_fd >= 0) close(w->to_fd);
    if (w->from_fd >= 0) close(w->from_fd);
    w->to_fd = w->from_fd = -1; w->child = -1; w->alive = 0; w->pending = 0; w->have = w->start = 0;
}

void sf_stop(void) {
    for (int i = 0; i < n_workers; ++i) stop_worker(&workers[i]);
    n_workers = 0;
}

static int send_line(Worker *w, const char *s) {
    size_t n = strlen(s), off = 0;
    while (off < n) {
        const ssize_t r = write(w->to_fd, s + off, n - off);
        if (r < 0) { if (errno == EINTR) continue; return 0; }
        off += (size_t)r;
    }
    return 1;
}

/* Next line (without the newline) into `out`; 0 on timeout or end of stream. */
static int read_line(Worker *w, char *out, size_t cap, int timeout_ms) {
    const double deadline = now() + timeout_ms / 1000.0;
    for (;;) {
        for (size_t i = w->start; i < w->have; ++i) {
            if (w->buf[i] == '\n') {
                size_t n = i - w->start < cap - 1 ? i - w->start : cap - 1;
                memcpy(out, w->buf + w->start, n); out[n] = '\0';
                w->start = i + 1;
                return 1;
            }
        }
        if (w->start > 0) { memmove(w->buf, w->buf + w->start, w->have - w->start); w->have -= w->start; w->start = 0; }
        if (w->have == sizeof w->buf) w->have = 0;   /* an absurd line: drop it */
        const int left = (int)((deadline - now()) * 1000);
        if (left <= 0) return 0;
        struct pollfd pfd = {w->from_fd, POLLIN, 0};
        const int r = poll(&pfd, 1, left);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return 0; }
        const ssize_t got = read(w->from_fd, w->buf + w->have, sizeof w->buf - w->have);
        if (got <= 0) { if (got < 0 && errno == EINTR) continue; return 0; }
        w->have += (size_t)got;
    }
}

static int wait_for(Worker *w, const char *prefix, int timeout_ms) {
    char line[4096];
    const double deadline = now() + timeout_ms / 1000.0;
    for (;;) {
        const int left = (int)((deadline - now()) * 1000);
        if (left <= 0 || !read_line(w, line, sizeof line, left)) return 0;
        if (strncmp(line, prefix, strlen(prefix)) == 0) return 1;
    }
}

static int launch(Worker *w, const char *path) {
    int in[2], out[2];
    signal(SIGPIPE, SIG_IGN);   /* a dead engine must not kill us */
    memset(w, 0, sizeof *w);
    w->to_fd = w->from_fd = -1; w->child = -1;
    if (pipe(in) != 0) return 0;
    if (pipe(out) != 0) { close(in[0]); close(in[1]); return 0; }
    w->child = fork();
    if (w->child < 0) { close(in[0]); close(in[1]); close(out[0]); close(out[1]); w->child = -1; return 0; }
    if (w->child == 0) {
        dup2(in[0], 0); dup2(out[1], 1);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    close(in[0]); close(out[1]);
    w->to_fd = in[1]; w->from_fd = out[0]; w->alive = 1;
    if (!send_line(w, "uci\n") || !wait_for(w, "uciok", 5000) ||
        !send_line(w, "setoption name Threads value 1\nsetoption name Hash value 16\nsetoption name MultiPV value 256\nisready\n") ||
        !wait_for(w, "readyok", 5000)) {
        stop_worker(w);
        return 0;
    }
    return 1;
}

/* MIST_SF_WORKERS processes (default 2: the arena gives two cores and the
 * search blocks on every call); all must start or none runs. */
int sf_start(const char *path) {
    snprintf(engine_path, sizeof engine_path, "%s", path);
    restarts = 0;
    sf_stop();
    int want = getenv("MIST_SF_WORKERS") ? atoi(getenv("MIST_SF_WORKERS")) : 2;
    if (want < 1) want = 1;
    if (want > MAX_WORKERS) want = MAX_WORKERS;
    for (int i = 0; i < want; ++i) {
        if (!launch(&workers[i], path)) { n_workers = i; sf_stop(); return 0; }
    }
    n_workers = want;
    return 1;
}

/* After a failure of one worker: start it again, at most three times a game;
 * otherwise Stockfish is off for the rest of the game. */
static int relaunch(Worker *w) {
    stop_worker(w);
    if (!engine_path[0] || restarts >= 3) { sf_stop(); return 0; }
    ++restarts;
    if (!launch(w, engine_path)) { sf_stop(); return 0; }
    return 1;
}

int sf_ready(void) { return n_workers > 0 && workers[0].alive; }
int sf_workers(void) { return sf_ready() ? n_workers : 0; }

void sf_stats(long *c, double *s) { *c = calls; *s = spent; }

void sf_fen(const Position *p, char out[100]) {
    static const char *letters[2] = {"PNBRQK", "pnbrqk"};
    char *o = out;
    for (int r = 7; r >= 0; --r) {
        int empty = 0;
        for (int f = 0; f < 8; ++f) {
            const int sq = r * 8 + f;
            if (p->piece[sq] == NO_PIECE) { ++empty; continue; }
            if (empty) { *o++ = (char)('0' + empty); empty = 0; }
            *o++ = letters[(int)p->color[sq]][(int)p->piece[sq]];
        }
        if (empty) *o++ = (char)('0' + empty);
        if (r) *o++ = '/';
    }
    *o++ = ' '; *o++ = p->side == WHITE ? 'w' : 'b'; *o++ = ' ';
    char *c0 = o;
    #define HOME(sq, t, c) (p->piece[sq] == (t) && p->color[sq] == (c))
    if ((p->castle_rooks & ((bb_t)1 << 7)) && HOME(4, KING, WHITE) && HOME(7, ROOK, WHITE)) *o++ = 'K';
    if ((p->castle_rooks & ((bb_t)1 << 0)) && HOME(4, KING, WHITE) && HOME(0, ROOK, WHITE)) *o++ = 'Q';
    if ((p->castle_rooks & ((bb_t)1 << 63)) && HOME(60, KING, BLACK) && HOME(63, ROOK, BLACK)) *o++ = 'k';
    if ((p->castle_rooks & ((bb_t)1 << 56)) && HOME(60, KING, BLACK) && HOME(56, ROOK, BLACK)) *o++ = 'q';
    #undef HOME
    if (o == c0) *o++ = '-';
    *o++ = ' ';
    if (p->ep >= 0) { *o++ = (char)('a' + p->ep % 8); *o++ = (char)('1' + p->ep / 8); }
    else *o++ = '-';
    snprintf(o, 20, " %d 1", p->halfmove > 0 ? p->halfmove : 0);
}

/* What Stockfish accepts: one king each, no pawn on a back rank, and the side
 * to move unable to take the other king (it refuses such a position with a
 * CRITICAL ERROR and stops answering). The search never asks for one: a move
 * that leaves the mover's king capturable is a terminal node. */
static int supported(const Position *p) {
    const bb_t kings = p->by_type[KING], pawns = p->by_type[PAWN];
    if (__builtin_popcountll(kings & p->by_color[WHITE]) != 1 || __builtin_popcountll(kings & p->by_color[BLACK]) != 1) return 0;
    if (pawns & 0xFF000000000000FFULL) return 0;
    return !is_attacked(p, king_square(p, p->side ^ 1), p->side);
}


int sf_submit(int worker, const Position *p) {
    if (!sf_ready() || worker < 0 || worker >= n_workers || !supported(p)) return 0;
    Worker *w = &workers[worker];
    if (w->pending) return 0;
    char fen[100], cmd[160];
    sf_fen(p, fen);
    snprintf(cmd, sizeof cmd, "position fen %s\ngo depth 1\n", fen);
    w->started = now();
    if (!send_line(w, cmd)) { relaunch(w); return 0; }
    w->pending = 1;
    return 1;
}

int sf_collect(int worker, const MoveList *l, int *cp, int *found) {
    char uci[256][6];
    for (int i = 0; i < l->count; ++i) { move_to_uci(l->moves[i], uci[i]); found[i] = 0; cp[i] = 0; }
    if (!sf_ready() || worker < 0 || worker >= n_workers) return 0;
    Worker *w = &workers[worker];
    if (!w->pending) return 0;
    w->pending = 0;
    int scored = 0;
    char line[4096];
    for (;;) {
        if (!read_line(w, line, sizeof line, 2000)) { relaunch(w); return 0; }
        if (strstr(line, "CRITICAL ERROR")) { relaunch(w); return 0; }
        if (strncmp(line, "bestmove", 8) == 0) break;
        if (strncmp(line, "info", 4) != 0 || !strstr(line, " multipv ")) continue;
        if (strstr(line, "lowerbound") || strstr(line, "upperbound")) continue;
        const char *s = strstr(line, " score "), *pv = strstr(line, " pv ");
        if (!s || !pv) continue;
        int value;
        if (strncmp(s + 7, "cp ", 3) == 0) value = atoi(s + 10);
        else if (strncmp(s + 7, "mate ", 5) == 0) {
            const int n = atoi(s + 12);
            value = n > 0 ? 20000 - 100 * n : -20000 - 100 * n;
        } else continue;
        char move[8] = "";
        sscanf(pv + 4, "%7s", move);
        for (int i = 0; i < l->count; ++i) {
            if (strcmp(uci[i], move) == 0) {
                if (!found[i]) ++scored;
                found[i] = 1; cp[i] = value;
                break;
            }
        }
    }
    ++calls; spent += now() - w->started;
    return scored;
}

int sf_children(const Position *p, const MoveList *l, int *cp, int *found) {
    if (!sf_submit(0, p)) {
        for (int i = 0; i < l->count; ++i) { found[i] = 0; cp[i] = 0; }
        return 0;
    }
    return sf_collect(0, l, cp, found);
}
