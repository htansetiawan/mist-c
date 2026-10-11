# Final fixed-tree solve interval

`MIST_M4_SETTLE=fraction` reserves part of the existing decision budget for
solver iterations without further tree expansion. It implements the scheduling
idea in [Obscuro section 3.4](https://arxiv.org/html/2506.01242v2#S3.SS4).
The range is 0 through 0.5; the default remains **0**. NaN, infinity and invalid
strings are rejected. The setting cannot change after a blueprint exists.

The original solver cutoff remains 75% of the decision budget; the growth
cutoff is `(0.75 - fraction) * budget`. Setup still consumes the same budget,
and snapshot time is unchanged. External evaluations during growth receive
the earlier cutoff. The iteration cap still applies and can end a decision
before its settling phase. `m4-settle` diagnostics record iterations, starting
node/expansion counts and elapsed settling time for both forced-prefix and
ordinary decisions.

Correctness: `make test_m4` passes. The new fixed-tree fixture verifies that
CFR continues while node/expansion counts stay unchanged, that the snapshot
is valid, and that invalid/midgame setting changes fail. This is a scheduling
check, not evidence of stronger play.

## Negative development pilot, 2026-10-11

Four searched-prefix runs compare fractions 0 and 0.2 at White decision 6 of
the seed12 full-game loss, with seeds 12 and 13 and reversed execution order.
Both settings choose `a4c3` in both seeds, leaving Qd4 attacked by hidden Nc6.
The 0.2 runs perform 1,825 and 1,691 fixed-tree iterations respectively, using
about 1.396 seconds each. All prefixes report success and valid snapshots;
final decisions report no deadline overrun. Prefix overrun is not separately
logged, so it is not asserted.

The trees can differ because forced-prefix wall-clock searches differ. This
does not compare extra solving on an identical frozen tree. It establishes
that the tested scheduling intervention did not fix this opening decision.
Do not enable by default or extend to a strength claim from these results.

Frozen candidate SHA-256:
`bb4daf445e23a7e28f7f83b56605f4d22e5209bd8a8e5e7a260944020f0b220c`.
Evidence: IAmAVirus `astra-submissions/analysis/m4-settle-20261011/`.
