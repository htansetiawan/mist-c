"""Export arena games as move lists for fitting the opponent move model.

Reads the arena replay mirror (one directory per game with listing.json and
replay.json, as fetched by the IAmAVirus ingest) and writes

    game <id> <first ply used> <white|-> <black|->
    <uci> <uci> ...

where a side is "-" when its moves are not used (random and baseline.py
players). Games are split by id into train/test files.

    python3 tools/export_moves.py RAW_GAMES_DIR OUT_PREFIX [--games N] [--since YYYY-MM-DD]
"""
import argparse
import hashlib
import json
import os
import random

EXCLUDED = {"Uniform", "Baseline", "baseline.py"}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("out_prefix")
    ap.add_argument("--games", type=int, default=2000)
    ap.add_argument("--since", default="2026-09-30")
    ap.add_argument("--test-share", type=float, default=0.15)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()
    ids = sorted(os.listdir(args.raw))
    random.Random(args.seed).shuffle(ids)
    files = {split: open(f"{args.out_prefix}-{split}.txt", "w") for split in ("train", "test")}
    kept = 0
    for gid in ids:
        if kept >= args.games:
            break
        try:
            with open(os.path.join(args.raw, gid, "listing.json")) as f:
                listing = json.load(f)
            if listing["createdAt"][:10] < args.since:
                continue
            white, black = listing["white"], listing["black"]
            if white in EXCLUDED and black in EXCLUDED:
                continue
            with open(os.path.join(args.raw, gid, "replay.json")) as f:
                replay = json.load(f)
        except (OSError, ValueError, KeyError):
            continue
        if replay.get("rulesVersion") != "fow-v1":
            continue
        moves = [fr["move"] for fr in replay["frames"][1:] if fr.get("move")]
        if len(moves) < 4:
            continue
        split = "test" if int(hashlib.sha1(gid.encode()).hexdigest(), 16) % 1000 < args.test_share * 1000 else "train"
        tag = lambda name: "-" if name in EXCLUDED else name.replace(" ", "_")
        files[split].write(f"game {gid} {tag(white)} {tag(black)}\n{' '.join(moves)}\n")
        kept += 1
    for f in files.values():
        f.close()
    print(f"exported {kept} games")


if __name__ == "__main__":
    main()
