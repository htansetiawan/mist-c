"""Build the regression suite from our recorded arena losses.

    python3 tools/suite_build.py RAW_DIR OUT.json [--min-version 26] [--opponents "A,B,..."] [--limit N]

RAW_DIR is the IAmAVirus replay store (games/<id>/replay.json, accepted.jsonl).
From every loss of ours (versions >= --min-version) to one of the opponents,
the suite keeps the decisions that decided it, each with the referee input
that reproduces it (the frames give the views and clocks exactly):

    fatal     our move left our king capturable on the true board
    material  our material lead dropped by 3+ points within two plies
    check     our king was attacked by a piece we could not see (the move
              played may or may not have escaped)

Each entry: game, opponent, version, side, k (our decision number, 1-based),
feed (input lines through the clock line of decision k), forced (our moves
before k, comma separated), board (true board, 64 chars, a8..h1 rows from
rank 1), played, kind, safe (a move existed that leaves the king safe on the
true board), swing (material points lost, material only).
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import chess  # noqa: E402  (the iamavirus venv has it)

VALUE = {"p": 1, "n": 3, "b": 3, "r": 5, "q": 9, "k": 0}
ABOVE = ["Greek Gift", "Knight Vision", "Decent Chess Bot", "Strongly Invisible", "duck", "Mist Opportunities", "Instructors"]


def board(b64: str, white_to_move: bool) -> chess.Board:
    rows = []
    for r in range(7, -1, -1):
        row, e = "", 0
        for f in range(8):
            c = b64[r * 8 + f]
            if c == "-":
                e += 1
            else:
                if e:
                    row += str(e)
                    e = 0
                row += c
        if e:
            row += str(e)
        rows.append(row)
    return chess.Board("/".join(rows) + (" w" if white_to_move else " b") + " - - 0 1")


def material(b64: str, white: bool) -> int:
    return sum(VALUE[c.lower()] for c in b64 if c != "-" and c.isupper() == white)


def king_safe_after(b: chess.Board, move: chess.Move, col: bool) -> bool:
    b2 = b.copy(stack=False)
    b2.push(move)
    k = b2.king(col)
    return k is not None and not b2.attackers(not col, k)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("out")
    ap.add_argument("--min-version", type=int, default=26)
    ap.add_argument("--opponents", default=",".join(ABOVE))
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()
    opponents = set(args.opponents.split(","))
    games = []
    for line in open(os.path.join(args.raw, "accepted.jsonl")):
        r = json.loads(line)
        for side, other in (("white", "black"), ("black", "white")):
            if r.get(side) == "IAmAVirus" and (r.get(side + "Version") or 0) >= args.min_version and r.get(other) in opponents:
                s = {"1-0": 1, "0-1": 0}.get(r["result"], 0.5)
                if (s if side == "white" else 1 - s) == 0:
                    games.append((r["id"], side, r[side + "Version"], r[other], r.get("createdAt") or ""))
    games.sort(key=lambda g: g[4])
    out = []
    for gid, side, ver, opp, created in games:
        try:
            rp = json.load(open(os.path.join(args.raw, "games", gid, "replay.json")))
        except Exception:
            continue
        f = rp["frames"]
        w = side == "white"
        ci = 0 if w else 1
        col = chess.WHITE if w else chess.BLACK
        feed = ["fow", f"{'w' if w else 'b'} 180 2"]
        forced = []
        k = 0
        for p in range(len(f) - 1):
            feed.append(f[p][side])
            if (p % 2 == 0) != w:
                continue
            feed.append("%.9f" % f[p]["clocks"][ci])
            k += 1
            played = f[p + 1]["move"]
            b = board(f[p]["board"], w)
            kinds = []
            kingsq = b.king(col)
            if kingsq is None:
                break
            attackers = b.attackers(not col, kingsq)
            unseen = bool(attackers) and all(f[p][side][sq] == "?" for sq in attackers)
            if unseen:
                kinds.append("check")
            mv = chess.Move.from_uci(played)
            safe_moves = [m for m in b.pseudo_legal_moves if king_safe_after(b, m, col)]
            # fog-of-war: a king in check may stay; pseudo-legal moves are the legal ones
            if mv in b.pseudo_legal_moves and not king_safe_after(b, mv, col):
                kinds.append("fatal")
            swing = 0
            if p + 2 < len(f):
                before = material(f[p]["board"], w) - material(f[p]["board"], not w)
                after = material(f[p + 2]["board"], w) - material(f[p + 2]["board"], not w)
                swing = before - after
                if swing >= 3 and "fatal" not in kinds:
                    kinds.append("material")
            if kinds:
                out.append({"game": gid, "opponent": opp, "version": ver, "side": side, "k": k, "ply": p,
                            "feed": "\n".join(feed) + "\n", "forced": ",".join(forced), "board": f[p]["board"],
                            "played": played, "kind": kinds[0], "kinds": kinds, "safe": bool(safe_moves), "swing": swing,
                            "created": created})
            forced.append(played)
        if args.limit and len(out) >= args.limit:
            break
    json.dump(out, open(args.out, "w"))
    kinds = {}
    for e in out:
        kinds[e["kind"]] = kinds.get(e["kind"], 0) + 1
    print(f"{len(games)} losses, {len(out)} decisions: {kinds}")


if __name__ == "__main__":
    main()
