"""Replay one of our decisions from a recorded referee game.

Feeds mist the referee's input lines for our side up to decision N, with our
earlier moves forced as they were played (MIST_FORCE_MOVES), and prints the
move it searches at decision N, repeated over seeds and settings.

    python3 tools/replay_decision.py REPLAY.json COLOUR N [--seeds 4] [--env K=V ...] [--mist build/mist]
"""
import argparse
import json
import os
import re
import subprocess
from collections import Counter


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("replay"); ap.add_argument("colour", choices=["white", "black"]); ap.add_argument("decision", type=int)
    ap.add_argument("--seeds", type=int, default=4)
    ap.add_argument("--env", nargs="*", default=[])
    ap.add_argument("--mist", default="build/mist")
    ap.add_argument("--seconds", default="3")
    args = ap.parse_args()
    game = json.load(open(args.replay))
    events = [e for e in game["protocol"]["events"] if e["side"] == args.colour]
    lines, played = [], []
    for e in events:
        if e["direction"] == "input":
            lines.append(e["text"])
        elif e["text"].startswith("move "):
            played.append(e["text"][5:])
            if len(played) == args.decision:
                break
    # the input up to (and including) the clock line of decision N
    clock_lines = [i for i, t in enumerate(lines) if re.fullmatch(r"[0-9.]+", t)]
    stop = clock_lines[args.decision - 1] + 1
    feed = "\n".join(lines[:stop]) + "\n"
    print(f"decision {args.decision}: played in the game {played[args.decision - 1]}")
    counts = Counter()
    for seed in range(1, args.seeds + 1):
        env = dict(os.environ, MIST_FORCE_MOVES=",".join(played[: args.decision - 1]), FOW_SEED=str(seed), MIST_MAX_SECONDS=args.seconds)
        for kv in args.env:
            k, v = kv.split("=", 1); env[k] = v
        out = subprocess.run([args.mist], input=feed, capture_output=True, text=True, env=env, timeout=120)
        moves = [l[5:] for l in out.stdout.splitlines() if l.startswith("move ")]
        info = [l for l in out.stderr.splitlines() if l.startswith("mist: n=")]
        counts[moves[-1] if moves else "?"] += 1
        print(f"  seed {seed}: {moves[-1] if moves else '?'}  {info[-1][6:] if info else out.stderr[-200:]}")
    print("  " + ", ".join(f"{m} x{c}" for m, c in counts.most_common()))


if __name__ == "__main__":
    main()
