#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L  /* fork, pipe, poll, kill, clock_gettime under -std=c11 */
/* mist-c: Stockfish over UCI. See sfeval.h. */
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

static int to_fd = -1, from_fd = -1, alive = 0, restarts = 0;
static char engine_path[512] = "";
static pid_t child = -1;
static char buf[1 << 16];
static size_t have = 0, start = 0;
static long calls = 0;
static double spent = 0;

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

void sf_stop(void) {
    if (child > 0) {
        if (alive && to_fd >= 0) { const char q[] = "quit\n"; ssize_t w = write(to_fd, q, sizeof q - 1); (void)w; }
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    if (to_fd >= 0) close(to_fd);
    if (from_fd >= 0) close(from_fd);
    to_fd = from_fd = -1; child = -1; alive = 0; have = start = 0;
}

static int send_line(const char *s) {
    size_t n = strlen(s), off = 0;
    while (off < n) {
        const ssize_t w = write(to_fd, s + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; return 0; }
        off += (size_t)w;
    }
    return 1;
}

/* Next line (without the newline) into `out`; 0 on timeout or end of stream. */
static int read_line(char *out, size_t cap, int timeout_ms) {
    const double deadline = now() + timeout_ms / 1000.0;
    for (;;) {
        for (size_t i = start; i < have; ++i) {
            if (buf[i] == '\n') {
                size_t n = i - start < cap - 1 ? i - start : cap - 1;
                memcpy(out, buf + start, n); out[n] = '\0';
                start = i + 1;
                return 1;
            }
        }
        if (start > 0) { memmove(buf, buf + start, have - start); have -= start; start = 0; }
        if (have == sizeof buf) have = 0;   /* an absurd line: drop it */
        const int left = (int)((deadline - now()) * 1000);
        if (left <= 0) return 0;
        struct pollfd pfd = {from_fd, POLLIN, 0};
        const int r = poll(&pfd, 1, left);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return 0; }
        const ssize_t got = read(from_fd, buf + have, sizeof buf - have);
        if (got <= 0) { if (got < 0 && errno == EINTR) continue; return 0; }
        have += (size_t)got;
    }
}

static int wait_for(const char *prefix, int timeout_ms) {
    char line[4096];
    const double deadline = now() + timeout_ms / 1000.0;
    for (;;) {
        const int left = (int)((deadline - now()) * 1000);
        if (left <= 0 || !read_line(line, sizeof line, left)) return 0;
        if (strncmp(line, prefix, strlen(prefix)) == 0) return 1;
    }
}

static int launch(const char *path);

int sf_start(const char *path) {
    snprintf(engine_path, sizeof engine_path, "%s", path);
    restarts = 0;
    return launch(path);
}

/* After a failure: start the engine again, at most three times a game. */
static int relaunch(void) {
    sf_stop();
    if (!engine_path[0] || restarts >= 3) return 0;
    ++restarts;
    return launch(engine_path);
}

static int launch(const char *path) {
    int in[2], out[2];
    signal(SIGPIPE, SIG_IGN);   /* a dead engine must not kill us */
    if (pipe(in) != 0) return 0;
    if (pipe(out) != 0) { close(in[0]); close(in[1]); return 0; }
    child = fork();
    if (child < 0) { close(in[0]); close(in[1]); close(out[0]); close(out[1]); return 0; }
    if (child == 0) {
        dup2(in[0], 0); dup2(out[1], 1);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    close(in[0]); close(out[1]);
    to_fd = in[1]; from_fd = out[0]; alive = 1;
    if (!send_line("uci\n") || !wait_for("uciok", 5000) ||
        !send_line("setoption name Threads value 1\nsetoption name Hash value 16\nsetoption name MultiPV value 256\nisready\n") ||
        !wait_for("readyok", 5000)) {
        sf_stop();
        return 0;
    }
    return 1;
}

int sf_ready(void) { return alive; }

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

int sf_children(const Position *p, const MoveList *l, int *cp, int *found) {
    if (!alive || !supported(p)) return 0;
    const double t0 = now();
    char uci[256][6];
    for (int i = 0; i < l->count; ++i) { move_to_uci(l->moves[i], uci[i]); found[i] = 0; cp[i] = 0; }
    char fen[100], cmd[160];
    sf_fen(p, fen);
    snprintf(cmd, sizeof cmd, "position fen %s\ngo depth 1\n", fen);
    if (!send_line(cmd)) { relaunch(); return 0; }
    int scored = 0;
    char line[4096];
    for (;;) {
        if (!read_line(line, sizeof line, 2000)) { relaunch(); return 0; }
        if (strstr(line, "CRITICAL ERROR")) { relaunch(); return 0; }
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
    ++calls; spent += now() - t0;
    return scored;
}
