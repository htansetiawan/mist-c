# Offline continuation traces

`MIST_M4_TRACE_PATH` enables a JSONL trace in the forced-replay frontend.
The ordinary arena path does not write it. `m4_trace_last()` reads the saved
blueprint after the move has been selected; it does not expand nodes or
update any strategy. Trace CPU/I/O time is reported separately. Do not use
instrumented end-to-end latency as a playing-clock benchmark.

For each current own-information-set board root, the trace includes its
effective alternate, counterfactual/opponent reaches, current strategy, board
and raw value. It then records the selected own move and every represented
opponent reply. Nodes distinguish proven terminals from unexpanded heuristic
frontiers and expanded decision nodes. Decision records count terminal losing
children; the independent referee, rather than those counts alone, establishes
which reply is forcing in the replay.

The exporter does not receive a true board or a nominated refutation. It dumps
all sampled roots; the offline Python auditor matches true roots afterward.
Boards include side, castling, en-passant and halfmove state. History hashes
remain the existing solver identities, never true-board labels. Blueprint
boundaries are artificial chance nodes: the exporter locates actual board
roots by current ply and own history instead.

A final record supplies root count, record count and completeness. Empty,
stale, invalid and truncated traces are not successful. The frontend caps
output at 20,000 records; a partial trace must not support an absence claim.

Validation: C tests exercise complete/truncated output and unchanged own
history, plus existing both-color snapshot/carry tests. The independent
referee audit checks 47,353 hypothetical node states in 16 traced decisions,
then reconstructs historical repetition counts for the actual-root labels.

Finding: in multiple `zn8m31zt` runs, the chosen e7f6 / c1c8 continuation has
all next own actions represented as terminal losses and raw value -1. The
root is present, but its heuristic alternate is already about -0.998.
Therefore missing expansion alone cannot explain that failure. This is not
proof that the solver knows which possible world is true, or that a simple
terminal gap improves playing strength. The next isolated experiment reserves
space between uncertain heuristic scores and proven terminal outcomes.

Repository evidence: `iamavirus/astra-submissions/analysis/m4-tree-audit-20261011/`.
