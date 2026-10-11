# Finite-search root dominance experiment

Disabled by default. `MIST_M4_DOMINANCE=1` enables a post-solve correction in
the experimental `mist-m4` target. This is an empirical extension, not a claim
that the Obscuro paper prescribes it or that it preserves equilibrium safety.

In the completed 48-position gate, s24/seed11 at leaf scale .9 chose e3d3
(fatal share .678431373, raw value -.971052423) although a4a5 had lower fatal
share .301960784 and no worse raw value -.970813019. The latter survives on
the recorded board. These rounded profiles motivated the experiment; they
are development evidence, not an independent strength test.

After the ordinary solve, obtain full-precision action values and fatal
shares from the saved continuation forest. Use its current counterfactual
gadget reach, not the belief posterior. A dominator must have strictly lower
fatal share by more than 1e-9 and no lower raw continuation value. Among
dominators choose minimum fatal share, then maximum value, then action order.
Sort the action frontier once and use binary search: O(A log A), A<=256.
Computing the profile still costs O(worlds × actions); changing the selection
data structure does not remove that necessary board work.

Push the entire purified played distribution through this mapping. Reuse the
original random draw by redirecting its selected action; do not draw again.
If any positive-support action moves, update the snapshot's root behavior to
the pushed distribution, recompute all reaches and values, and only then
commit the move. A previously zero-support target now has positive saved
reach. Counterfactual branches are retained; regret accumulators are not
reused. Off-current root behavior and the opponent strategy stay unchanged.

The baseline snapshot stores unpurified current behavior. This experiment
stores corrected played behavior at the root only when a redirect occurs.
That carry change is part of the intervention; on-policy comparisons must
measure it, not infer their results from post-hoc move labels.

Profile work is optional and must finish before 85% of the decision budget.
Never use a partially scanned root set. An incomplete/invalid profile or
failed retarget leaves the original move and snapshot behavior unchanged.
The final 15% is reserved, not a hard-real-time guarantee: inherited forest
traversals are not interruptible. Record overruns and test normal clocks.

Tests:
- `make test_m4_policy`: compare the sorted frontier with exhaustive selection,
  preserve probability/value invariants, preserve outputs on invalid inputs,
  redirect to a zero-support action and carry its resulting own reach.
- `MIST_M4_DOMINANCE=1 ./build/test_m4_chess`: both-color chess carry and timing.
- `make test_m4`: existing gadget, rules-adapter, evaluator and carry invariants.

No default change or submission is warranted by correctness tests. Next:
recheck the motivating position; inspect a new full-game loss; then fresh
paired games at the same clock. Full-game tests use frozen binaries.
