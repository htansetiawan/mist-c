"""Test vectors for milestone M1 from the referee-verified rules (iamavirus learning/rules.py).

    IAMAVIRUS_ROOT=~/git/iamavirus python tests/gen_vectors.py 400 > tests/vectors.txt

One line per position: board side castle_mask ep | sorted legal moves | white view | black view.
Random games from the start position until the mover leaves its king capturable or 200 plies.
"""
import os, random, sys
sys.path.insert(0, os.path.expanduser(os.environ.get("IAMAVIRUS_ROOT", "~/git/iamavirus")))
from learning import rules

games = int(sys.argv[1]) if len(sys.argv) > 1 else 200
random.seed(7)
for g in range(games):
    b = rules.Board.from_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1")
    for ply in range(200):
        moves = sorted(b.legal_moves())
        print(f"{b.board_string()} {b.side} {b.castling} {b.en_passant} | {' '.join(moves)} | {b.observation(0)} {b.observation(1)}")
        if not moves:
            break
        b.push(random.choice(moves))
        if b.mover_lost():
            break
