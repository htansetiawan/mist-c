/* mist-c: Stockfish as the leaf evaluator (paper, Section 3.3: "the node
 * heuristic is an estimate of the perfect-information value of z, as evaluated
 * by the chess engine Stockfish"; Appendix C.5: "in MultiPV mode, at depth 1 on
 * node z, which gives evaluations for all children of z in a single call").
 * Stockfish runs as a separate process over UCI. Any failure (no binary, a
 * timeout, a dead process) turns it off for the rest of the game and the
 * search falls back to evaluate(). */
#ifndef MIST_SFEVAL_H
#define MIST_SFEVAL_H

#include "fow.h"

/* Starts the engine at `path`; 1 if it answered uciok and readyok. */
int sf_start(const char *path);
int sf_ready(void);
/* Scores of the moves of `l` in `p` from the side to move's point of view, in
 * centipawns (mates as +-20000 less 100 per move). found[i] is 0 where
 * Stockfish gave no score: Fog of War moves that are not legal chess, such as
 * castling through an attacked square. Returns how many were scored (0 on
 * failure). */
int sf_children(const Position *p, const MoveList *l, int *cp, int *found);
/* The same in two halves over several worker processes (MIST_SF_WORKERS):
 * sf_submit sends `p` to `worker` (1 if it is now in flight; 0 when the
 * position is not supported, the worker is busy or Stockfish is off) and
 * sf_collect waits for that worker's answer and scores `l` as sf_children
 * does. sf_workers() is how many workers can take a request (0 = off). */
int sf_workers(void);
/* Lines a call scores at most (MIST_SF_MULTIPV): a call that scores this
 * many has ranked the rest below them. */
int sf_multipv(void);
int sf_submit(int worker, const Position *p);
int sf_collect(int worker, const MoveList *l, int *cp, int *found);
/* Which workers hold a complete answer, as a bit mask over the workers in
 * flight; waits up to timeout_ms for the first (0: just look). A worker in
 * the mask answers sf_collect without blocking. sf_pending: a request is in
 * flight on `worker`. */
int sf_poll(int timeout_ms);
int sf_pending(int worker);
/* Calls made and their total wall time, for the decision log. */
void sf_stats(long *calls, double *seconds);
void sf_stop(void);
/* FEN of `p` (castling only where king and rook are home). */
void sf_fen(const Position *p, char out[100]);

#endif
