# Observation-grouped escape sets

`src/m4escape.c/.h` is an original bounded diagnostic primitive. It is not
linked into `mist-m4` and does not change its policy, leaf values or clocks.
It is not another claim about the Obscuro gadget's safety guarantees.

## The observed failure

Two completed public games, `9pl31sez` and `burzt1so`, contain the same sequence:

```
1. e4 d5 2. exd5 Qxd5 3. d4 Qa5 4. Be3??
```

In the MIT arena, White immediately loses after Be3 because its king remains
attacked. The queen on a5 is invisible. More interestingly, **Qe4 and Qe6
produce the same White observation as Qa5**, from the same preceding board.
The queen has vacated d5, but its destination is not observed.

Against Qa5, seven replies survive the next move. Against Qe4 or Qe6, five
survive. Those sets are disjoint. Every individual world has an escape, yet
there is no single deterministic response that works for the shared observation.
Any randomized reply has survival probability at most 1/2 against at least
one of the disjoint pair: probability mass assigned to disjoint escape sets
cannot sum to more than one. This is a worst-case statement for that subset,
not the opponent's actual choice probability.

The original Python referee oracle checks all 29 legal White moves one turn
earlier. d3 and d4 produce such a conflict; Ke2 permits a physically forced
loss. The other 26 have an observation-conditioned safe reply after every
nonterminal opponent move at this short horizon, from the recorded board.
Nc3 both develops a piece and blocks the a5 diagonal; Be2 closes the e-file.
This is not a claim that all 26 moves are equally good or safe over a game.

Full evidence and Python scripts live in IAmAVirus under
`astra-submissions/analysis/duck-opening-alias-20261011/`.

## Interface and semantics

Create an accumulator for one candidate/context, then add nonterminal positions
at our future turn. Give indistinguishable histories the same `history` tag;
give distinguishable histories different tags. The key additionally checks
the complete own observation, side and own castling rights. Hidden opponent
move names must not be used to distinguish otherwise identical histories.

For each state s, compute its legal replies that avoid immediate king loss,
S(s). Maintain the intersection over each information group I:

```
C(I) = intersection over s in I of S(s)
```

`common.count == 0` and `individually_dead == 0` identifies an information
conflict in the represented states. `individually_dead > 0` instead includes
a physically forced loss. Nonempty `common` covers the states actually added;
it says nothing about missing belief worlds or more distant tactics.

The caller must exclude terminal positions, including **historical threefold
draws**, which a `Position` alone cannot identify. Missing kings, last-mover
exposure, no legal moves and halfmove >=100 are rejected locally. An invalid
input or capacity failure permanently marks `complete` false. It does not
erase earlier conflict witnesses. There is no posterior weighting or risk
probability in this API.

## Cost and ownership

All table/group storage is allocated once. Expected lookup is O(1) with
full-key collision checks and a table load below 1/2; adversarial collisions
can require a linear probe. Sorting at most 256 move codes costs O(A log A),
and intersecting two sorted lists costs O(A). Storage is bounded by the requested
group count, with no retained boards or unbounded history. Input positions are
not modified. The caller bounds the number of added worlds or its own deadline.

## Checks

```
make test_m4_escape build/escape_probe
```

The C tests cover the recorded disjoint escape sets, a synthetic color-reflected
equivalent, distinct remembered histories/own castling rights, order invariance,
duplicates, immutable input, capacity/invalid-state handling and a physically
forced loss. Address, undefined-behavior and leak sanitizers also pass on the
host; LeakSanitizer cannot run under the sandbox's ptrace supervision.

`tools/escape_probe.c` accepts complete boards on stdin for **offline** checks.
The independent Python referee cross-check agrees on all 1,307 nonterminal
successor boards across 537 observation groups after the 29 earlier legal
moves. This establishes conformance for that family, not Elo or runtime strength.

## Next experiment

Use the primitive to label representation/expansion failures before tuning
an evaluation penalty. A future planner could prioritize expansion of
observation groups with incompatible escapes, or learn an earlier-move
feature from these labels. Any runtime use must consume belief/search states
only, preserve perfect recall, report incomplete coverage, and earn its cost
in the fixed replay and real-clock match gates. It must not consume the
diagnostic's true board or silently assume all hidden threats are equally likely.
