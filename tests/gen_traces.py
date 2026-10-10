"""Belief traces from real arena games (milestone M2), from our side's point of view.

    IAMAVIRUS_ROOT=~/git/iamavirus python tests/gen_traces.py 40 > tests/traces.txt

Per game: "game <id> <w|b>", then per ply: "<us|them> <uci> <our view> <true board> <side> <castle mask> <ep>".
"""
import glob, json, os, sys
root = os.path.expanduser(os.environ.get("IAMAVIRUS_ROOT", "~/git/iamavirus"))
sys.path.insert(0, root)
from learning import rules

limit = int(sys.argv[1]) if len(sys.argv) > 1 else 30
done = 0
for path in sorted(glob.glob(os.path.join(root, "data/raw/games/*/listing.json"))):
    l = json.load(open(path))
    for side, color in (("white", 0), ("black", 1)):
        if l.get(side) != "IAmAVirus" or l.get(f"{side}Version") not in (20, 21):
            continue
        r = json.load(open(path.replace("listing.json", "replay.json")))
        moves = [f["move"] for f in r["frames"][1:] if f["move"]]
        b = rules.Board.from_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1")
        print(f"game {l['id']} {'w' if color == 0 else 'b'}")
        for m in moves:
            who = "us" if b.side == color else "them"
            b.push(m)
            print(f"{who} {m} {b.observation(color)} {b.board_string()} {b.side} {b.castling} {b.en_passant}")
        done += 1
        if done >= limit:
            sys.exit(0)
