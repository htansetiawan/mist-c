#!/bin/bash
cd "$(dirname "$0")"
# Stockfish, when shipped in bin/, scores the search's leaves (paper, 3.3)
if [ -x bin/stockfish ]; then export MIST_STOCKFISH="$PWD/bin/stockfish"; fi
exec ./mist
