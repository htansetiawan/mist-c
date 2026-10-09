# fowsearch-c

A clean-room C implementation of imperfect-information search for Fog of War (dark) chess, written from the Obscuro paper (Zhang & Sandholm, arXiv:2506.01242, ICLR 2026). Goal: an engine small enough for a 2-core, 4 GiB arena, and a testbed for exploiting and improving equilibrium search (learned belief sampling, risk-aware selection against belief dilution).

Status: specification and milestone M1 (rules, visibility, observations) in progress. See `docs/SPEC.md`.

License: to be decided by the maintainers (written from the paper only; no code from GPL implementations).
