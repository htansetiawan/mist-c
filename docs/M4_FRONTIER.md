# Tactical information-set expansion experiment

Disabled by default. `MIST_M4_CLOSURE=1` enables it in `mist-m4`;
the C API is `m4_set_tactical_closure`. This is a scheduling experiment,
not a new leaf evaluator, risk penalty, opponent prior or safety guarantee.

The Duck opening audit contains indistinguishable Qa5 and Qe4 worlds whose
safe replies are disjoint. Expanding only one world's future decision can
leave its aliases represented by static leaf values. The experiment spends
part of tree growth exposing replies across that shared information set.

Every newly created live own-turn leaf enters a bounded index keyed by its
existing perfect-recall own-history hash and ply. If the own king is attacked
in any member, every queued member becomes eligible, including quiet worlds.
Hidden opponent move names are not part of the key. This uses possible search
boards, never the replay's true board. Existing M4 infoset sharing still
determines the common strategy after expansion.

A min-heap schedules shallower groups first, with stable insertion-order
ties. Each group keeps a FIFO of node IDs. Half the expansion opportunities
try this queue; the rest retain the existing search schedule. An empty queue
falls back immediately. Popped nodes already expanded by ordinary search are
skipped. New aliases added after a group's queue drains remain eligible.

Hash lookup is expected O(1); a heap insertion/removal is O(log G), FIFO pop
O(1). The index stores at most 65,536 entries per decision, bounded by the
configured node cap. It performs no repeated full-tree scan: one initial
scan registers carried leaves; later expansions register their new children.
Overflow drops scheduling entries and is reported, while ordinary search
continues. It does not truncate the game tree or the belief.

The deadline, node limit, terminal rules, scores, gadget, and final policy
selection remain the existing M4 implementation. The index is rebuilt for
each solve and never transfers old regrets or queue state into a new game.
Groups are only partially expanded if the clock or node cap binds. A quiet
world absent from the tree cannot be recovered by this index. History hashes
have the same 64-bit collision assumption as the existing M4 adapter.
Existing repetition and conservative en-passant limitations are unchanged.

`m4-closure` diagnostics record scheduled entries, successful priority
expansions, and capacity drops, separately on prefix and final decisions.
Observe these counters before interpreting a result as a test of the idea.

Validation:

- `test_m4_frontier`: quiet aliases before/after a threat, distinct histories
  and plies, ordering, reinsertion after draining, and capacity exhaustion.
- `test_m4_chess`: actual threat expansions with a retained snapshot; existing
  legal-play/carry tests with the flag enabled, both colors and evaluators.
- Address/undefined-behavior/leak sanitizer run of enabled chess integration.
- Paired replay pilot still required; no playing-strength claim or release.

Run: `make test_m4`, then `MIST_M4_CLOSURE=1 ./build/test_m4_chess`.
Set `MIST_STOCKFISH` to test the separate bounded Stockfish evaluator too.
