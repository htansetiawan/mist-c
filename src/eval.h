/* mist-c: leaf evaluation (our own; the paper uses Stockfish, which we do not ship).
 * Material, piece placement, king shelter, each king's lines into the fog
 * (squares its side cannot see) and the best immediate capture for the side
 * to move, in centipawns, squashed to [-1, 1] for the solver. */
#ifndef MIST_EVAL_H
#define MIST_EVAL_H

#include "fow.h"

/* Centipawns from `us`'s point of view; the capture term belongs to p->side. */
int evaluate_cp(const Position *p, int us);
/* tanh(cp / 400): the value the solver sees. */
double evaluate(const Position *p, int us);
/* Lines into the fog around `color`'s king: directions that run over empty
 * squares into a square `color` does not see (where the enemy still has a
 * piece moving that way), and unseen empty knight squares (if it has
 * knights), as {lines, knight squares}. */
void eval_fog(const Position *p, int color, int out[2]);

#endif
