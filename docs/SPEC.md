# fowsearch-c: specification (clean-room, from the paper only)

Source: B. H. Zhang and T. Sandholm, *General search techniques without common knowledge for imperfect-information games, and application to superhuman Fog of War chess* (Obscuro), arXiv:2506.01242v2 / ICLR 2026. This implementation is written from the paper alone. It does not use or translate code from any other implementation (in particular not from GPL-licensed ones).

## Rules (paper, Appendix A)
- Win by capturing the king; no check or checkmate. Moving into or failing to escape a check is legal and loses. Castling into, out of, or through check is legal. Stalemate is a win for the stalemating player. No insufficient-material draw.
- After every move each player observes every square onto which her pieces can legally move. A pawn blocked by an enemy piece does not observe the blocker. A pawn that can capture en passant sees the pawn it can capture.
- Threefold repetition and the 50-move rule end the game as draws without a claim.

## Algorithm (paper, Section 3 and Appendix C)
1. **Belief.** Keep the set P of every position consistent with our observations (usually |P| ≤ 10^6; average 17,264 over 1,000 games). Update it exactly after each observation.
2. **Sample.** Each turn sample I ⊆ P uniformly, |I| at most a few hundred. *(Paper's future work, and our first improvement: sample from P with a learned opponent model instead of uniformly.)*
3. **Subgame (2-KLUSS, C.6).** Build the subgame from the previous turn's tree (the blueprint) plus I: remove nodes outside the downward closure of the order-2 knowledge set; keep our nodes at distance 1 *unfrozen* and re-optimize them. Nodes newly added from P assume the opponent has perfect information; their alternate value is min{v̂(h), v*} (v̂: evaluation of h, v*: our value from the previous search).
4. **Gadgets (B.2, C.1–C.3).** Build both *Resolve* and *Maxmargin* gadget games over the same tree. Alternate values u(x,y|J) (conditional value under the blueprint), gift values ĝ(J) = Σ_{J'a' ⪯ J} [u^cf(x,y;J'a') − u^cf(x,y;J')]^+ (reach subgame solving). Resolve's root distribution is α(J) = ½ (y(J)/Σ_J' y(J') + 1/m). Use Maxmargin while the opponent always exits in the Resolve gadget (the current strategy is safe), otherwise Resolve; the switch can happen mid-search.
5. **Equilibrium (3.2, C.7).** Predictive CFR+ (PCFR+) over the expanded tree; act on the **last iterate**, not the average.
6. **Expansion (3.3, C.4): one-sided GT-CFR.** The exploring player alternates each expansion. The non-exploring player plays its current CFR strategy y^t; the explorer plays x̃ = ½ x̃_Max + ½ x̃_PUCT, where x̃_Max is uniform over the support of its current strategy and x̃_PUCT plays argmax_a Q̃(I,a) = u(x^t,y^t|I,a) + C·σ^t(I,a)·√N^t(I)/(1+N^t(I,a)), C = 1, σ the empirical std of past visits' values (seeded with samples −1 and +1). A leaf reached under (x̃, y^t) is expanded.
7. **Leaf evaluation (C.5).** Evaluate all children of the expanded node in one call (the paper: Stockfish 14 MultiPV depth 1, clamped to [−1,1]). A new infoset's regret minimizer starts with all weight on its best child. *(The paper's simple evaluation, material plus squares visible to each side, still beat the prior state of the art 55%; our arena bot cannot ship Stockfish, so the evaluator is pluggable.)*
8. **Threads and time (3.4).** One CFR thread, two expansion threads (expansion uses locks; CFR works lock-free on the expanded part). Budget from the remaining clock; stop expansion first, then CFR after a delay.
9. **Move selection (3.5, C.8): purification.** Under Maxmargin, mix among at most 3 *stable* actions (in the support for every iteration after T_{1/2}); under Resolve, play the top action. Mass of excluded actions goes to the top action.

Reference numbers (D): 116.6 plies per game, search depth 10.7 plies, 1.07M nodes and 14,404 infosets per search, 181k nodes carried over, at 5 s per move.

## Milestones
- **M1 rules:** bitboards, Fog of War move generation (pseudo-legal, king capture, castling without check rules, en passant, promotions), make/unmake, visibility, observation strings in the course arena's format. Tested against the referee-verified Python rules.
- **M2 belief:** exact P with incremental update and enumeration cap; measured against true boards in recorded games.
- **M3 search core:** game tree with infosets, PCFR+, one-sided GT-CFR, pluggable evaluator, purification.
- **M4 subgame solving:** 2-KLUSS, Resolve/Maxmargin gadgets, carry-over.
- **M5 arena:** course protocol front end, time manager, two threads within 2 cores and 4 GiB.
- **M6 experiments:** against the open-source Misty locally; ablations; our improvements (learned sampling of I, risk-aware selection against belief dilution).
