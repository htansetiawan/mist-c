/* mist-c: imperfect-information game trees and Predictive CFR+ (milestone M3).
 *
 * The paper (Section 3.2, Appendix C.7) solves each subgame with PCFR+ and acts
 * on the LAST iterate rather than the average. This module is the solver on a
 * generic two-player zero-sum tree with information sets and perfect recall:
 * chance, player and terminal nodes; utilities are for player 0. The Fog of War
 * subgame builder (M3/M4) produces such a tree; tests/test_cfr.c checks the
 * solver on Kuhn poker, whose equilibrium value for player 0 is -1/18.
 */
#ifndef MIST_CFR_H
#define MIST_CFR_H

#include <stddef.h>

enum { NODE_CHANCE = 0, NODE_PLAYER = 1, NODE_TERMINAL = 2 };

typedef struct {
    int type, player;      /* player: 0 or 1 at player nodes */
    int infoset;           /* player nodes: index of the information set */
    int first, count;      /* children: edges [first, first + count) */
    double value;          /* terminal nodes: utility for player 0 */
} GameNode;

typedef struct {
    GameNode *nodes; int n_nodes, cap_nodes;
    int *child; double *prob; int n_edges, cap_edges;   /* edge -> child node; chance probability */
    int *is_player, *is_actions, *is_offset; int n_infosets, cap_infosets;
    int n_slots;                                         /* total (infoset, action) pairs */
    int finalized_slots;                                 /* slots the solver arrays cover */
    double *regret, *last, *strategy, *strategy_sum;     /* per slot */
    int root;
} Game;

void game_init(Game *g);
void game_free(Game *g);
int  game_infoset(Game *g, int player, int actions);    /* returns the infoset index */
int  game_node(Game *g, int type, int player, int infoset, double value);
/* Children are added in action order; for chance nodes `p` is the probability. */
void game_children(Game *g, int node, const int *children, const double *p, int count);
void game_finalize(Game *g);                             /* allocates solver arrays */
/* GT-CFR grows the tree between solver iterations: after adding infosets and
 * nodes, game_grow extends the solver arrays (new slots start at zero regret)
 * and game_expand_leaf turns a terminal leaf into a player node in place, so
 * every node index stays valid and the solver keeps what it learned. */
void game_grow(Game *g);
void game_expand_leaf(Game *g, int node, int player, int infoset, const int *children, int count);

/* One iteration of alternating PCFR+: update player 0, then player 1. */
void pcfr_iteration(Game *g, int t);
/* Current (last-iterate) behaviour strategy of every infoset into `out` (n_slots). */
void pcfr_current(const Game *g, double *out);
/* Average strategy (weighted by iteration) into `out`. */
void pcfr_average(const Game *g, double *out);
/* Expected utility for player 0 when both play `strategy` (n_slots). */
double game_value(const Game *g, const double *strategy);
/* Best-response value for `player` against `strategy` (player 0's utility if
 * player 0 best-responds, else player 1's), and exploitability = sum of both
 * players' best-response gains over the strategy profile's value. */
double best_response_value(const Game *g, const double *strategy, int player);
double exploitability(const Game *g, const double *strategy);

#endif
