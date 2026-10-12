#!/bin/bash
cd "$(dirname "$0")"
# Stockfish, when shipped in bin/, scores the search's leaves (paper, 3.3)
if [ -x bin/stockfish ]; then export MIST_STOCKFISH="$PWD/bin/stockfish"; fi
# the empirical opponent prior, when shipped in data/ (tools/history_table.c)
if [ -f data/history.bin ]; then export MIST_HISTORY_TABLE="$PWD/data/history.bin"; fi
exec ./mist
