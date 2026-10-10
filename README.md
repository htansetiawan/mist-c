# mist-c

A clean-room C implementation of imperfect-information search for Fog of War (dark) chess, written from the Obscuro paper: B. H. Zhang and T. Sandholm, *General search techniques without common knowledge for imperfect-information games, and application to superhuman Fog of War chess*, arXiv:2506.01242 (ICLR 2026).

**Goals:**

- an engine small enough for a 2-core, 4 GiB tournament arena (the MIT 6.7980 Fog of War arena);
- a testbed for attacking and improving equilibrium search in imperfect information, such as learned sampling of the belief and risk-aware move selection against belief dilution.

**Clean room.** The code is written from the paper only. It contains no code from GPL implementations; in particular it does not derive from Misty, the GPL-3.0 open implementation of Obscuro. Misty is used only as a separate opponent in local experiments.

## Status

| milestone | what | state |
| --- | --- | --- |
| M1 rules | bitboards, Fog of War move generation (king capture wins; no check rules; castling, en passant, promotions), make/unmake, visibility, arena observation strings | **done**: 13,467 positions, 0 mismatches against the referee-verified rules |
| M2 belief | the exact set P of positions consistent with our observations, updated incrementally with an enumeration cap | **done**: sound on 40 real arena games (3,137 plies). The true position is never missing while P is exact. **Weighted (v4):** positions the referee's rule excludes are dropped (a move leaving the mover's king capturable ends the game); every hidden opponent move is scored by an opponent move model (`src/movemodel.c`, a conditional logit fitted by `tools/fit_move_model.py` on 2,029 arena games; on 371 held-out games it gains 0.63 nats per move over uniform, top-1 27.7%); positions reached by several histories add their weights; past the cap positions are kept by weighted sampling, and search draws its worlds by weight. On the 40 traces (cap 100k): the truth's weight share is 2.6x uniform's, it is in a 64-world sample 60% of the time (uniform 50%), and the cap loses it at 276 plies instead of 524 |
| M3 search core | game tree with infosets, PCFR+ (last iterate), one-sided GT-CFR, pluggable evaluator, purification | **GT-CFR first version** (`src/gtcfr.c`): the tree grows between PCFR+ rounds where the alternating explorer's current strategy and a visit bonus lead; information sets keyed by each player's own history; about 310k nodes per move in the arena budget; beats `baseline.py` as both colours. Earlier: the PCFR+ solver on a generic tree is done (`src/cfr.c`). On Kuhn poker its value converges to -1/18 and its last iterate is unexploitable (to 1e-5) by iteration 100. A first decision procedure (`src/search.c`) samples I from P, builds the two-ply belief subgame (our move in one infoset, the opponent's reply in infosets keyed by its view), and plays the PCFR+ last iterate. On 57 decisions from recorded arena games: 0 illegal moves, never leaves our king capturable on the true board, 37 ms per decision. Next: GT-CFR expansion and an arena front end |
| M4 subgame solving | 2-KLUSS, Resolve/Maxmargin gadgets, carry-over between turns | |
| M5 arena | referee protocol front end, time manager, two threads in 2 cores / 4 GiB | **first version** (`src/main.c`, `make build/mist`): speaks the course referee protocol, keeps the exact belief from every view, decides with the two-ply PCFR+ search, falls back to the visible position if the capped belief loses the truth. First games on the course referee: 4 wins of 4 against uniform random and `baseline.py`, as White and as Black. Then: history-aware opponent infosets, reservoir capping, our own leaf evaluator (material, placement, king shelter, best capture) and a time-bounded solver; 2/2 against `baseline.py` in 42 and 47 plies. With GT-CFR (v1) on the local course referee: 7-1-2 against the TA-P replica and 4-3-3 against IAmAVirus v25. Two of those losses were illegal moves as Black after the capped belief had lost every position and the fallback treated unseen squares as empty (a pawn pushed into an unseen piece). v2: the belief is rebuilt from the view when it empties (`belief_resample`), and no move is sent unless the view allows it on every board (`view_moves`); `tests/test_guard.c` checks both on 1,616 recorded decisions and on the two arena positions, as White and as Black. With the belief capped at 50 to force the rebuild, v2 won 2/2 against the TA-P replica on 0.5 s per move (3 and 7 rebuilds, no guard needed). v3: three v1 draws were threefold repetitions, two of them seven points up; when ahead by 2+ the front end leaves out root moves that would make our pieces stand as they already have twice (`src/front.c`, tested as White and as Black in `tests/test_front.c` with capture counting, castling rights and the last-resort move) |
| M6 experiments | against Misty locally; ablations; our improvements | Local course referee, 180 s + 2 s: v3 (GT-CFR, guard, repetition rule) 4-0-2 against IAmAVirus v25 and 6-1-1 against the TA-P replica, but 0-8 against the open-source Misty: outplayed on material in half the games, and caught by hidden checks (a queen or rook on an unseen line) while the search was confident in the rest. v4 (weighted belief) is being measured against Misty, v3 and v25 |

The full specification, with paper section references, is in [`docs/SPEC.md`](docs/SPEC.md).

## A first measurement: how big the belief gets

The paper reports an average |P| of 17,264 over 1,000 games and "usually |P| ≤ 10^6". On 40 recorded arena games between strong bots (`tests/traces.txt`) the exact belief is larger:

| cap on \|P\| | games that overflow the cap | mean \|P\| | mean update (one thread) |
| --- | --- | --- | --- |
| 100,000 | 24 of 40 | 11,796 | 0.023 s |
| 1,000,000 | 10 of 40 | 42,946 | 0.089 s |

Past the cap, an enumerating engine must drop positions and can lose the true one. In these games that happened 442 times at the 100k cap and 161 times at the 1M cap.

## Build and test

```sh
make test   # rules conformance (tests/vectors.txt) and belief soundness on the arena traces at a 100k cap
./build/test_belief tests/traces.txt 1000000   # the 1M measurement above (about 5 minutes)
```

Requires a C11 compiler. The test data is generated by the scripts in `tests/` from a separate rules implementation (`IAMAVIRUS_ROOT`); the generated files are committed, so the tests run standalone.

`tests/traces.txt` holds 40 public games of the IAmAVirus team from the course arena, written from our side: each ply's move, our view, the true board, the side to move, castling rights and the en passant square.

## License

Not yet chosen.
