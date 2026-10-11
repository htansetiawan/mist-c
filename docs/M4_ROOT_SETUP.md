# Carried-root initialization: capacity and evaluator cost

The first clock-controlled pair against Fable v8 scored 1–1, but the Black
win included a search failure at ply17. M4 carried 7,041 current roots and
1,614 off-current roots. Initialization made 6,050 external evaluator calls
(4.71 seconds) before the 5.02-second setup cutoff, returned `ok=0`, and the
frontend used its visible-board fallback. The win is retained, not treated as
a clean M4 strength result.

There are two distinct costs to fix:

1. **Mandatory capacity preflight.** The copied forest can fit its node cap
   while lacking room for complete current-root action rows. Before admitting
   a carried closure, count those required children and reserve an upper
   bound for fresh roots plus gadget wrappers. If it cannot fit, reuse the
   existing fallback4 path: restart M4 from sampled belief worlds. Do not
   truncate the knowledge closure or wait until half its rows are built.
2. **Opt-in cheap initialization.** `MIST_M4_FAST_ROOTS=1` omits external leaf
   evaluation only while first expanding carried current frontier roots.
   The in-process evaluator still supplies their nonterminal initial values;
   rules still adjudicate terminals. Normal search expansion and fresh-world
   child-backed alternate values still use the configured external evaluator.
   This changes heuristic initialization and needs a strength comparison.

The ordinary default still uses external evaluation during carried-root
initialization. Capacity preflight is enabled for both settings.

A real-Stockfish adapter test exposed the capacity issue at a 30,000-node
cap: Black decision4, carry1153/offI644, failed after794 cheap initializations.
With preflight it instead restarts from12 belief roots, builds9,108 nodes,
finishes40 solver iterations and saves a valid snapshot in about0.21seconds.
Both colors and the unchanged gadget/carry tests are checked separately.

A searched-prefix replay of the actual ply17 game is a diagnostic, not an
exact saved-state reproduction: earlier wall-clock solves may construct a
different carried tree. Preserve both profiles and all prefix failures.
