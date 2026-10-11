# Nonterminal value-gap experiment

Default is unchanged: `MIST_M4_LEAF_SCALE=1`. The single tested alternative is
0.9. `m4_set_leaf_scale()` accepts finite values in (0,1] before the first saved
blueprint, and rejects a value change afterward. This prevents mixing carried
blueprints trained under different utility semantics within a game.

The multiplier applies to uncertain static root evaluations and to every live
child's internal/Stockfish heuristic. Proven wins, losses, 50-move draws and
no-move terminals retain their existing exact utilities. Child-backed fresh
alternates are computed from those same children; subsequent blueprints keep
the same semantics. This is an experimental value transformation, not a claim
that a score of .9 is a calibrated 95% win probability.

Why test it: a trace of a real losing seat found the selected move's refutation
fully expanded to terminal loss -1, but its heuristic reference was already
about -.998. Standard-chess evaluation, including mate scores, is not itself
a FOW terminal certificate. Reserving a gap tests whether saturation causes
the solver to exchange actual losses for small uncertain evaluation gains.

Validation: both-color exact wins/losses remain +/-1 in `test_m4_scale`;
draws remain zero in `test_m4_eval`; invalid settings and midgame changes are
rejected. Scaled internal and real-Stockfish on-policy legality, snapshot and
carry plumbing checks pass in both colors. These are correctness checks.

The preregistered 32-decision development pilot passed its compute screen:
forced-loss prevention 2/8 -> 7/8; d01 immediate survival 3/4 -> 4/4;
s18 immediate survival 2/4 -> 3/4. No new immediate control loss. All decisions
validate and retain snapshots, with no reported M4 overrun or Stockfish failure.
Two seeds and evaluators on the same seat are correlated. This is not an Elo
estimate, nor evidence against an arena baseline. One Black prevention and one
internal s18 failure remain. No sweep of alternate scale values was performed.

Evidence and preregistration:
`iamavirus/astra-submissions/analysis/m4-tree-audit-20261011/leaf-gap-pilot.json`.
A paired 384-decision broad gate, searched-prefix checks, current-baseline
comparisons and 180+2 on-policy matches remain required. No release yet.
