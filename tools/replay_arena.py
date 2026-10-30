"""Replay one of our decisions from an ARENA replay (data/raw/games/<id>/replay.json) through mist.
    MIST_STOCKFISH=... MIST_HISTORY_TABLE=... python3 tools/replay_arena.py REPLAY.json COLOUR PLY [--seeds 2] [--seconds 4] [--mist build/mist] [--env K=V ...]
PLY is the 1-based ply of our move to re-decide (even plies for Black). The engine receives exactly the protocol
input our side saw up to that decision, with our earlier moves forced (MIST_FORCE_MOVES), and the true boards for
MIST_TRUTH_SEQ so it logs the truth's share and rank in its belief. Prints the move it would play per seed, the game's
move, the decision line, the truth line and the guard lines.
"""
import argparse, json, os, re, subprocess
ap = argparse.ArgumentParser()
ap.add_argument("replay"); ap.add_argument("colour", choices=["white", "black"]); ap.add_argument("ply", type=int)
ap.add_argument("--seeds", type=int, default=2); ap.add_argument("--seconds", default="4"); ap.add_argument("--mist", default="build/mist")
ap.add_argument("--env", nargs="*", default=[])
a = ap.parse_args()
f = json.load(open(a.replay))["frames"]; w = a.colour == "white"; ci = 0 if w else 1
assert ((a.ply - 1) % 2 == 0) == w, "ply must be our side's move"
feed = ["fow", f"{'w' if w else 'b'} 180 2"]; forced = []
for p in range(a.ply):
    feed.append(f[p][a.colour])
    if (p % 2 == 0) != w: continue
    feed.append("%.9f" % f[p]["clocks"][ci]); forced.append(f[p + 1]["move"])
forced = forced[:-1]
env = dict(os.environ, MIST_FORCE_MOVES=",".join(forced), MIST_MAX_SECONDS=a.seconds, MIST_TRUTH_SEQ=",".join(x["board"] for x in f[:a.ply]))
for kv in a.env:
    k, v = kv.split("=", 1); env[k] = v
for s in range(1, a.seeds + 1):
    env["FOW_SEED"] = str(s)
    out = subprocess.run([a.mist], input="\n".join(feed) + "\n", capture_output=True, text=True, env=env, timeout=300)
    moves = re.findall(r"^move (\S+)", out.stdout, re.M); err = out.stderr.splitlines()
    pick = lambda pre: [l for l in err if l.startswith(pre)][-1:]
    print(f"seed {s}: engine {moves[-1] if moves else '?'}  game {f[a.ply]['move']}  | " + " | ".join(pick("mist: n=") + pick("mist: truth") + [l for l in err if "guard left out" in l][-2:]))
