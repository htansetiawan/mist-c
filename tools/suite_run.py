"""Run one engine variant over the regression suite and score it on the true board.

    python3 tools/suite_run.py SUITE.json OUT.json [--mist build/mist] [--env K=V ...]
                               [--seconds 2.5] [--workers 6] [--kinds material,check,fatal]
                               [--limit N] [--seed 1] [--stockfish PATH] [--truth RAW_DIR]

Each suite decision is replayed (our earlier moves forced, the referee input
reproduced from the frames) and the searched move is scored on the true board:

    fatal     the move leaves our king capturable (a certain loss)
    cp_loss   Stockfish (depth 8) score of the best move minus the score after
              ours, from our side, capped at 1000 (skipped when the position
              has no side-to-move king capture to make and is otherwise not a
              normal chess position)
    repeat    the move is the one played in the recorded game

The summary prints, per kind, the fatal rate (with a safe move available),
the mean and median cp loss and the repeat rate. Compare variants on the
same suite and seed: the decisions are paired.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from multiprocessing import Pool

import chess
import chess.engine

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from suite_build import board, king_safe_after  # noqa: E402

ARGS = None
ENGINE = None


def init(args):
    global ARGS, ENGINE
    ARGS = args
    ENGINE = chess.engine.SimpleEngine.popen_uci(args.stockfish)
    ENGINE.configure({"Threads": 1, "Hash": 32})


def score_cp(b: chess.Board, col: bool):
    """(best, after-move) centipawns from our side, or None if Stockfish cannot score it."""
    global ENGINE
    if b.king(chess.WHITE) is None or b.king(chess.BLACK) is None:
        return None
    them = b.king(not col)
    if b.attackers(col, them):
        return None  # we can take the king: the move to make is that capture
    for attempt in range(2):
        try:
            info = ENGINE.analyse(b, chess.engine.Limit(depth=8))
            return info["score"].pov(col).score(mate_score=1000)
        except Exception:
            # Stockfish stops answering on some fog positions: relaunch once
            try:
                ENGINE.close()
            except Exception:
                pass
            ENGINE = chess.engine.SimpleEngine.popen_uci(ARGS.stockfish)
            ENGINE.configure({"Threads": 1, "Hash": 32})
    return None


def truth_seq(entry):
    """The true boards of the game through the decision's view, for MIST_TRUTH_SEQ."""
    rp = json.load(open(os.path.join(ARGS.truth, "games", entry["game"], "replay.json")))
    return ",".join(f["board"] for f in rp["frames"][:entry["ply"] + 1])


def run_one(entry):
    env = dict(os.environ, MIST_FORCE_MOVES=entry["forced"], MIST_STOCKFISH=ARGS.stockfish,
               MIST_MAX_SECONDS=str(ARGS.seconds), FOW_SEED=str(ARGS.seed))
    if ARGS.truth:
        env["MIST_TRUTH_SEQ"] = truth_seq(entry)
    for kv in ARGS.env:
        k, v = kv.split("=", 1)
        env[k] = v
    try:
        out = subprocess.run([ARGS.mist], input=entry["feed"], capture_output=True, text=True, env=env, timeout=120)
    except subprocess.TimeoutExpired:
        return {"id": entry["game"] + ":" + str(entry["k"]), "error": "timeout"}
    moves = [l[5:].strip() for l in out.stdout.splitlines() if l.startswith("move ")]
    if len(moves) != entry["k"]:
        return {"id": entry["game"] + ":" + str(entry["k"]), "error": f"{len(moves)} moves for decision {entry['k']}: {out.stderr[-300:]}"}
    move = moves[-1]
    w = entry["side"] == "white"
    col = chess.WHITE if w else chess.BLACK
    b = board(entry["board"], w)
    mv = chess.Move.from_uci(move)
    res = {"id": entry["game"] + ":" + str(entry["k"]), "kind": entry["kind"], "kinds": entry["kinds"], "safe": entry["safe"],
           "played": entry["played"], "move": move, "repeat": move == entry["played"],
           "fatal": not king_safe_after(b, mv, col), "guard": "king guard" in out.stderr}
    info = [l for l in out.stderr.splitlines() if l.startswith("mist: n=")]
    if info:
        res["info"] = info[-1][6:]
    truth = [l for l in out.stderr.splitlines() if l.startswith("mist: truth")]
    if truth:
        # the belief after the last update: is the true board in it, with what share, at what rank
        m = re.search(r"share=([\d.]+) rank=(\d+)", truth[-1])
        res["truth_lost"] = "LOST" in truth[-1]
        if m:
            res["truth_share"] = float(m.group(1))
            res["truth_rank"] = int(m.group(2))
    best = score_cp(b, col)
    if best is not None:
        b2 = b.copy(stack=False)
        b2.push(mv)
        after = score_cp(b2, col) if b2.king(col) is not None and not b2.attackers(not col, b2.king(col)) else -1000
        if after is not None:
            res["cp_loss"] = max(0, min(1000, best - after))
    return res


def summarize(results):
    kinds = sorted({r["kind"] for r in results if "kind" in r})
    lines = []
    for kind in ["all"] + kinds:
        rs = [r for r in results if "kind" in r and (kind == "all" or r["kind"] == kind)]
        if not rs:
            continue
        safe = [r for r in rs if r["safe"]]
        cps = sorted(r["cp_loss"] for r in rs if "cp_loss" in r)
        tr = [r for r in rs if "truth_lost" in r]
        if tr:
            ranks = sorted(r.get("truth_rank", 10**9) for r in tr)
            shares = sorted(r.get("truth_share", 0.0) for r in tr)
            lines.append(f"{kind:9s} truth in P {sum(not r['truth_lost'] for r in tr)}/{len(tr)} | rank med {ranks[len(ranks) // 2]} p25 {ranks[len(ranks) // 4]} | share med {shares[len(shares) // 2]:.4f} | in top 10: {sum(r.get('truth_rank', 10**9) <= 10 for r in tr) / len(tr):.3f}")
        lines.append(f"{kind:9s} n {len(rs):4d} | fatal {sum(r['fatal'] for r in safe)}/{len(safe)} ({sum(r['fatal'] for r in safe) / max(len(safe), 1):.3f} where a safe move existed)"
                     f" | cp loss mean {sum(cps) / max(len(cps), 1):6.1f} median {cps[len(cps) // 2] if cps else 0:4d} (n {len(cps)})"
                     f" | repeats the game move {sum(r['repeat'] for r in rs) / len(rs):.3f}")
    errors = [r for r in results if "error" in r]
    if errors:
        lines.append(f"errors {len(errors)}: {errors[0]['error'][:200]}")
    return "\n".join(lines)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("suite")
    ap.add_argument("out")
    ap.add_argument("--mist", default="build/mist")
    ap.add_argument("--env", nargs="*", default=[])
    ap.add_argument("--seconds", type=float, default=2.5)
    ap.add_argument("--workers", type=int, default=6)
    ap.add_argument("--kinds", default="material,check,fatal")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--stockfish", default=os.environ.get("MIST_STOCKFISH", "bin/stockfish"))
    ap.add_argument("--truth", default="", help="replay store: also track the true board's share and rank in the belief")
    args = ap.parse_args()
    kinds = set(args.kinds.split(","))
    suite = [e for e in json.load(open(args.suite)) if e["kind"] in kinds]
    if args.limit:
        suite = suite[:args.limit]
    with Pool(args.workers, initializer=init, initargs=(args,)) as pool:
        results = pool.map(run_one, suite, chunksize=1)
    json.dump({"mist": args.mist, "env": args.env, "seconds": args.seconds, "seed": args.seed, "results": results}, open(args.out, "w"))
    print(summarize(results))


if __name__ == "__main__":
    main()
