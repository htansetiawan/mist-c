# Native structural trap labels

`tools/m4_dilemma_labels.c` accelerates the Python information-dilemma labeler.
It is an offline data generator, not a playing policy. The active engine does
not link it.

From one complete position, enumerate every legal setup move and every opposing
reply. Group continuing replies by the resulting defender observation. For
each group, count how many worlds each response survives. A group with no
response that survives every world is a one-response information conflict.
This also includes physically forced losses; it is not by itself proof of
incompatible but individually escapable threats. Use the Python forensic
labeler to recover safe sets, disjoint-defense witnesses and local minimax
certificates.

The generic pattern is demonstrated by `e4 d5 exd5 Qxd5 d4`: a queen on a5
and one on e6 give the same observation, but their nonempty defense sets are
disjoint. Their equal mixture forces at least one-half immediate loss in this
conditional response game. This is not a full-game value or duck-policy claim.

## Data contract

Build with `make build/m4_dilemma_labels`. Each stdin line is:

```text
position_id side castling_rooks_hex ep_square halfmove_clock board64
```

Side is 0/1, squares use a1=0, `ep_square=-1` means absent. Board symbols match
the public API. Input positions must be nonterminal, have both kings, and come
from a validated legal history. Output is one JSON row per setup action,
including observation-group summaries, immediate terminals, surviving reply
count and the best uniform-reply immediate-loss fraction. The uniform fraction
is a diagnostic, not a learned opponent probability. Draws and opponent moves
that already terminate the game are separated from continuing reply groups.

Repetition history is not modeled; reject conclusions that depend on it.
For an online interpretation, first prove the starting belief is a singleton
without previous pruning, thinning or collapse. Oracle replay truth alone does
not establish this. The current corpus records that provenance separately.
Treat nonzero process exit as failure, including any partial JSON output.

## Cost and validation

Replies are sorted by 64-character observation in O(R log R), not compared
against every existing group. Response counts use the encoded move as a direct
array index; generation stamps avoid clearing 32,768 entries for every group.
King-exposure tests still require examining each legal response in each world.
Storage is bounded by the existing 256-move list limit plus the move-code table.

The independent Python comparison checks all 719 actions in the frozen
20-position opening corpus, plus both-color en-passant, castling, promotion and
50-move boundary examples: 30 positions, 807 actions total. Exact agreement
covers observations, reply/response counts, shared safe-action counts, minimum
fatal counts and immediate outcomes. Optimized run: 0.104550 CPU seconds on this
host. ASan/UBSan output is identical, with no reported fault. This is throughput
and rules evidence, not an Elo or runtime-engine improvement.

The Python comparator lives in IAmAVirus under
`astra-submissions/tools/test_m4_dilemma_native.py`. Full witnesses remain in
`astra-submissions/analysis/m4-cap-and-dilemmas-20261011/`.
