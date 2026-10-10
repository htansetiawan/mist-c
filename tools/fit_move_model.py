"""Fit the opponent move model (conditional logit) by Newton's method.

    python3 tools/fit_move_model.py train.bin test.bin [--cheap] [--movers movers.txt]

Reads tools/move_features.c output. Reports per-move log-likelihood (nats)
against the uniform model, top-1 accuracy, per mover on the test set, and
prints theta as a C initialiser for src/movemodel.c.
"""
import argparse
import struct
import sys

import numpy as np

NAMES = ["capture", "captured_value", "king_capture", "knight", "bishop", "rook", "queen", "king",
         "double_push", "castle", "promote_queen", "underpromote", "forward", "retreat", "centre",
         "develop", "recapture", "advanced_pawn", "check", "escape", "into_attack", "hang_value",
         "self_capturable"]
ATTACK = slice(18, 23)


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    F = struct.unpack_from("<i", data, 0)[0]
    off = 4
    xs, played, counts, movers = [], [], [], []
    while off < len(data):
        n, k, mover = struct.unpack_from("<iii", data, off)
        off += 12
        x = np.frombuffer(data, dtype="<f4", count=n * F, offset=off).reshape(n, F)
        off += 4 * n * F
        xs.append(x); played.append(k); counts.append(n); movers.append(mover)
    X = np.concatenate(xs).astype(np.float64)
    counts = np.array(counts)
    starts = np.concatenate([[0], np.cumsum(counts)[:-1]])
    return X, starts, counts, np.array(played), np.array(movers)


def stats(theta, X, starts, counts, played):
    z = X @ theta
    zmax = np.maximum.reduceat(z, starts)
    seg = np.repeat(np.arange(len(starts)), counts)
    e = np.exp(z - zmax[seg])
    Z = np.add.reduceat(e, starts)
    logp_played = z[starts + played] - zmax - np.log(Z)
    p = e / Z[seg]
    return logp_played, p, seg, z


def objective(theta, X, starts, counts, played, l2):
    logp = stats(theta, X, starts, counts, played)[0]
    return logp.mean() - 0.5 * l2 * float(theta @ theta)


def fit(X, starts, counts, played, l2=1e-3, iters=40):
    """Newton's method with backtracking on the mean log-likelihood (L2-penalised)."""
    F = X.shape[1]
    N = len(starts)
    theta = np.zeros(F)
    current = objective(theta, X, starts, counts, played, l2)
    for it in range(iters):
        logp, p, seg, z = stats(theta, X, starts, counts, played)
        mu = np.add.reduceat(p[:, None] * X, starts)                  # per position
        grad = (X[starts + played] - mu).sum(0) / N - l2 * theta
        H = ((X * p[:, None]).T @ X - mu.T @ mu) / N + l2 * np.eye(F)
        step = np.linalg.solve(H, grad)
        alpha = 1.0
        while alpha > 1e-4:
            trial = theta + alpha * step
            value = objective(trial, X, starts, counts, played, l2)
            if value >= current:
                break
            alpha *= 0.5
        theta, gain, current = trial, value - current, value
        print(f"   iteration {it}: objective {current:.5f} (step {alpha:g})", file=sys.stderr)
        if gain < 1e-7:
            break
    return theta


def report(label, theta, X, starts, counts, played, movers=None, names=None):
    logp, p, seg, z = stats(theta, X, starts, counts, played)
    uniform = -np.log(counts)
    best = np.maximum.reduceat(z, starts)
    top1 = (z[starts + played] >= best - 1e-12).mean()
    print(f"{label}: {len(starts)} positions; log-likelihood per move {logp.mean():.3f} nats vs uniform {uniform.mean():.3f} "
          f"(gain {logp.mean() - uniform.mean():+.3f}); top-1 {top1:.3f}; mean legal moves {counts.mean():.1f}")
    if movers is not None and names:
        for mid in sorted(set(movers.tolist())):
            sel = movers == mid
            if sel.sum() < 300:
                continue
            print(f"   {names.get(mid, mid):24s} n={sel.sum():6d} gain {logp[sel].mean() - uniform[sel].mean():+.3f} nats/move")
    return logp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("train"); ap.add_argument("test")
    ap.add_argument("--cheap", action="store_true", help="leave out the attack features")
    ap.add_argument("--movers", help="stderr of move_features for the test file (mover names)")
    args = ap.parse_args()
    Xtr, str_, ctr, ptr, mtr = load(args.train)
    Xte, ste, cte, pte, mte = load(args.test)
    if args.cheap:
        Xtr[:, ATTACK] = 0; Xte[:, ATTACK] = 0
    theta = fit(Xtr, str_, ctr, ptr)
    names = {}
    if args.movers:
        for line in open(args.movers):
            if line.startswith("mover "):
                _, i, name = line.split(maxsplit=2)
                names[int(i)] = name.strip()
    report("train", theta, Xtr, str_, ctr, ptr)
    report("test", theta, Xte, ste, cte, pte, mte, names)
    for name, t in zip(NAMES, theta):
        print(f"   {name:16s} {t:+.3f}")
    print("static const float theta[MM_FEATURES] = {" + ", ".join(f"{t:.4f}f" for t in theta) + "};")


if __name__ == "__main__":
    main()
