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
/* Calls made and their total wall time, for the decision log. */
void sf_stats(long *calls, double *seconds);
void sf_stop(void);
/* FEN of `p` (castling only where king and rook are home). */
void sf_fen(const Position *p, char out[100]);

#endif
