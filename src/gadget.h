/* M4: paper-only subgame gadgets (Obscuro B.2, C.1-C.3, Fig. 9).
 * All values, including alternate values and gifts, use PLAYER ZERO utility.
 * The caller supplies a disjoint frontier and its opponent information sets.
 * These must be history-based; a current board/view alone is insufficient.
 */
#ifndef MIST_GADGET_H
#define MIST_GADGET_H
#include "cfr.h"

typedef struct {
    int node, group;            /* source node; dense opponent-group index */
    double cf_reach;            /* chance * player-0 reach, NOT posterior mass */
} GadgetRoot;

typedef struct {
    double alternate;           /* blueprint u(x,y|J), in player-0 units */
    double gift;                /* nonnegative CF gift; effective alt = alt-gift */
    double opponent_reach;      /* y(J), once per group, NOT summed over worlds */
} GadgetGroup;

enum { GADGET_RESOLVE = 0, GADGET_MAXMARGIN = 1 };
typedef struct {
    Game game;                 /* one shared continuation, two root layers */
    int resolve_root, maxmargin_root, selector;
    int groups, source_nodes, source_infosets;
    int *node_map, *infoset_map; /* source -> retained copy, -1 if discarded */
    int *entry_infoset, *group_chance;
    double *alpha, *offset;     /* group prior; effective alt in player-0 units */
} Gadget;

/* Initializes out on success or failure; free with gadget_free. Rejects
 * overlapping roots, non-trees, malformed probabilities and zero CF support.
 * Copies only the selected descendants; original infosets remain shared.
 * Nodes are centered by effective alt. Resolve exits with 0, so its value is
 * the truncated margin objective; Maxmargin directly selects a group.
 * Call gadget_iteration for the smooth controller, or pcfr_iteration with
 * an explicitly selected fixed gadget for stationary-game tests.
 */
int gadget_build(const Game *source, const GadgetRoot *roots, int n_roots,
                 const GadgetGroup *groups, int n_groups, Gadget *out);
void gadget_free(Gadget *g);
void gadget_mode(Gadget *g, int mode);
/* Figure 10 controller. Both root minimizers learn on every iteration; the
 * continuation has one shared set of regrets. Mode selection above is only
 * for fixed-gadget tests and has no effect on this automatic controller. */
typedef struct {
    double *strategy, *instant, *root_weight, *root_value;
    unsigned char *visited;
    int slots, infosets, groups; /* workspace capacities, grows with the tree */
    double max_enter, active_mass;
} GadgetSolver;
void gadget_solver_init(GadgetSolver *s);
void gadget_solver_free(GadgetSolver *s);
/* Effective opponent root reach, NOT a normalized chance distribution:
 * w[J] = pmax*alpha[J]*enter[J] + (1-pmax)*maxmargin[J].
 * Returns pmax, or NAN for invalid inputs; weights has groups entries. */
double gadget_root_weights(const Gadget *g,const double *strategy,double *weights);
/* One alternating PCFR+ iteration, then both auxiliary root updates. Returns
 * 0 on bad/unfinalized state or allocation failure. Use pcfr_current for the
 * resulting continuation behavior; solver.root_weight/max_enter/active_mass
 * give the new controller state. Call game_grow after expanding the tree.
 * This computes a shared adaptive search policy, not an equilibrium solver
 * for a single stationary game; benchmark fixed gadgets separately. */
int gadget_iteration(Gadget *g,int iteration,GadgetSolver *workspace);
/* Expanded leaves must retain their group's offset; never mix uncentered
 * eval scores with centered gadget terminals. Returns raw - effective_alt. */
double gadget_leaf_value(const Gadget *g, int group, double raw);
/* Starts fresh regret minimizers with the blueprint as first behaviour.
 * Cumulative regret and averages are zero. Initial prediction is set to x
 * until first visited, then overwritten with the first instantaneous regret.
 * Does not reuse the old solver's regret, prediction or average statistics. */
int gadget_seed_blueprint(Gadget *g, const Game *source, const double *strategy);
/* C.2 incremental gift over an opponent action on the path to the cut. */
double gadget_gift_step(double gift, double before_cf, double after_cf);
/* C.1 alternate value for a newly sampled singleton opponent infoset. */
double gadget_new_alternate(double leaf, double previous_value);

/* Evaluate a fixed blueprint on a tree. Arrays have source->n_nodes entries.
 * Outputs: expected continuation values, chance*P0 reach, P1-only reach.
 * Structurally unreachable nodes get value 0 and reach 0. Zero-probability
 * opponent branches are still evaluated counterfactually. No BR assumption.
 * Use the group helper to normalize the counterfactual reach exactly once. */
int gadget_blueprint_stats(const Game *source, const double *strategy,
                           double *value, double *cf_reach, double *opp_reach);
/* A carried adaptive game is a forest below the artificial gadget layers.
 * Preserve the actual opponent root reach without turning it into chance or
 * normalizing it. Each boundary is a disjoint subtree root; nodes outside
 * these subtrees have zero reach/value. Zero opponent reach retains CF support.
 */
typedef struct { int node; double cf_reach, opponent_reach; } GadgetBoundary;
int gadget_forest_stats(const Game *source, const double *strategy,
                        const GadgetBoundary *boundary, int count,
                        double *value, double *cf_reach, double *opp_reach);
int gadget_group_blueprint(const GadgetRoot *roots, int n_roots, int group,
                           const double *value, const double *opp_reach,
                           double *alternate, double *opponent_reach);
#endif
