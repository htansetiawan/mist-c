# Observation-aware opponent move model (belief build step 3), 29 October 2026

Question: does the opponent move model get sharper when it knows what the mover can see? The
belief weights each world by the model's likelihood of the opponent's moves, so a sharper model
raises the true world's share (the sharpness curve: a share of 0.2 per decision reaches the 80%
bar against v35; the natural belief sits at a median of 0.003).

## What was added (mist-c 92627fd, branch belief-obs)

Eleven features of a move relative to the mover's own visibility (`visible_squares` of the mover
in the true position, computed once per position and passed to `move_features_obs`), appended to
the 28 dense features; zero weights leave the model unchanged (`make test` passes with the zero
header). Attack-class features, so they belong to the full model (used while |P| <= 10,000):

| feature | meaning | fitted weight |
|---|---|---:|
| obs_escape_seen | the moved piece is attacked by a visible enemy piece | +0.960 |
| obs_into_attack_seen | the destination is attacked by a visible enemy piece | -0.167 |
| obs_hang_seen | ... times the moved piece's value / 9 | -3.082 |
| obs_capture_defended_seen | a capture on a square a visible enemy piece attacks | +0.682 |
| obs_self_capturable_seen | leaves own king attackable by a visible enemy piece | -1.682 |
| obs_threat_seen | the moved piece attacks a visible enemy piece worth 3+ | -0.483 |
| obs_approach_seen | closes the distance to the nearest visible enemy piece, / 7 | +0.164 |
| obs_reveal | squares newly seen after the move, / 16 | +1.573 |
| obs_threat_seen_value | the most valuable visible piece attacked, / 9 | +1.867 |
| obs_capture_defended_unseen | a capture on a square only an unseen enemy piece attacks | +0.176 |
| obs_fog_exposure | unseen squares from which a queen or knight could reach the destination, / 27 | -2.164 |

Three king-seen features were tried first and dropped: in Fog of War the mover sees the enemy
king only when it can capture it, so they were zero on every row.

Reading the weights: a bot hangs a piece to an attacker it can see far less often (hang_seen
-3.08 on top of hang_value -0.62) than to one it cannot; it rarely leaves its king to a visible
attacker (-1.68 on top of -2.99); it moves into vision (reveal +1.57) and away from squares
exposed to the fog (-2.16); it attacks what it sees, weighted by value (+1.87).

## Held-out fit (tools/fit_move_model_pst.py, 8 epochs, Adam, L2 1e-4 on the tables)

Train 6,776 arena games since 2026-10-05 (509,476 positions), test 1,224 games (92,418
positions); exports `{storage}/iamavirus-data/movemodel/arena-{train,test}-obs.bin`
(42 columns: 39 dense, piece, mirrored from, mirrored to). Nats per move over uniform and top-1
on the test set:

| model | nats over uniform | top-1 |
|---|---:|---:|
| compiled before this work (28 dense, no tables) | 0.623 | 0.261 |
| control: 28 dense + piece-square tables | 0.759 | 0.279 |
| 39 dense (with observation) + piece-square tables | 0.789 | 0.282 |
| cheap (no attack or observation features) + tables | 0.602 | 0.257 |

Per opponent (test positions >= 2,000):

| mover | positions | PST nats | PST+obs nats | PST top-1 | PST+obs top-1 |
|---|---:|---:|---:|---:|---:|
| Decent_Chess_Bot | 8396 | 0.814 | 0.848 | 0.280 | 0.286 |
| Strongly_Invisible | 7553 | 0.752 | 0.790 | 0.262 | 0.270 |
| Instructors | 7340 | 0.705 | 0.741 | 0.262 | 0.258 |
| duck | 7138 | 0.864 | 0.896 | 0.306 | 0.308 |
| Greek_Gift | 6505 | 0.789 | 0.826 | 0.271 | 0.279 |
| Knight_Vision | 6231 | 0.823 | 0.859 | 0.274 | 0.271 |
| IAmAVirus | 5989 | 0.717 | 0.742 | 0.275 | 0.280 |
| TA_Baseline_A | 5438 | 0.983 | 1.007 | 0.332 | 0.339 |
| TA_Baseline_S | 5401 | 0.882 | 0.920 | 0.311 | 0.320 |
| HYPOTHERMIA | 5237 | 0.632 | 0.673 | 0.239 | 0.240 |
| TA_Baseline_P | 4992 | 0.900 | 0.924 | 0.315 | 0.318 |
| Mist_Opportunities | 4422 | 0.801 | 0.836 | 0.306 | 0.310 |
| Lost_Signal | 4225 | 0.741 | 0.766 | 0.273 | 0.285 |
| Apricot_Squad | 3013 | -0.039 | -0.053 | 0.152 | 0.157 |
| Gravel_Road | 2993 | 0.737 | 0.757 | 0.298 | 0.299 |
| Sausage | 2664 | 0.754 | 0.779 | 0.276 | 0.277 |
| In_Pawn_Sight | 2268 | 0.623 | 0.637 | 0.259 | 0.264 |

## Truth share on our v35 games (the number that matters)

`MIST_THREADS=1 ./build/test_belief tests/traces-v35.txt 200000` (60 arena games of v35,
5,172 plies; the share is the true world's weight in the belief while it is exact, |P| > 1,
3,327 plies; "64-world sample" = how often a 64-draw sample would contain the truth):

| header | truth share, geometric mean | vs uniform | in a 64-world sample | truth lost after the cap |
|---|---:|---:|---:|---:|
| zero tables (the compiled model) | 0.0063 | x1.8 | 55.7% | 112 plies |
| piece-square tables (control) | 0.0086 | x2.5 | 59.8% | 76 plies |
| piece-square + observation | 0.0084 | x2.4 | 60.6% | 78 plies |

The piece-square tables raise the truth's weight by about a third; the observation features
add nothing measurable on top (within a few percent either way) although they gain 0.03 nats
on the held-out set. Neither is near the 0.2 target: the share needed is some twenty times
what any linear move model gets. The fitted observation header is installed in
`src/movemodel_theta.h` on this branch (best held-out model, same truth share as the control);
the control header is `docs/belief-obs-20261029/movemodel_theta_pst.h`, the fit summary
`fit.json`.

## Commands

    taskset -c 6 make build/move_features build/test_belief
    build/move_features {storage}/movemodel/arena-train.txt {storage}/movemodel/arena-train-obs.bin 1   # and test
    python tools/fit_move_model_pst.py TRAIN-obs.bin TEST-obs.bin --control --no-dense-refit --epochs 8 \
        --threads 1 --out theta_obs.h --out-control theta_pst.h --summary fit.json \
        --movemodel build/move_features --test-txt TEST.txt        # 2.5 min on one core
    MIST_THREADS=1 ./build/test_belief tests/traces-v35.txt 200000   # 7 min on one core per header
