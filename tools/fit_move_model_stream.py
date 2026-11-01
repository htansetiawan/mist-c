#!/usr/bin/env python3
"""Fit the opponent move model on a feature file of any size.

A conditional logit over the mover's legal moves, P(m | s) = softmax(theta . phi(s, m)),
dense facts only (no piece-square tables), fitted by Newton's method with the feature
file streamed from disk in chunks, so the file may be far larger than memory.

    python3 tools/fit_move_model_stream.py TRAIN.bin TEST.bin --names NAMES.txt
        [--skip-mover IAmAVirus] [--init src/movemodel.c] [--passes 6] [--ridge 1e-2]
        [--fix 2,26,27] [--cheap] [--chunk-mb 256] [--out fit.json]

Reads tools/move_features.c output: int32 F; per position int32 n, int32 played,
int32 mover_id, float32 x[n][F], with the dense facts in the first F - 3 columns.
NAMES.txt holds the "mover <id> <name>" lines the feature tool prints on stderr.
Positions whose mover is --skip-mover are left out of the fit and of the headline
test figures (they are reported on their own). --fix lists columns held at 0;
--cheap holds columns 18 and up at 0 (the cheap model). --init reads theta_full from a
movemodel.c or movemodel_theta.h so Newton starts near the optimum.

Reports nats per move over uniform and top-1 on the test set, overall and per mover,
for the initial weights and after every pass, and writes the fitted theta as a C array.

TRAIN and TEST may be named pipes fed by tools/move_features once per pass (one run
per open), so the feature file never has to exist on disk; give --columns then.
"""
import argparse
import json
import re
import struct
import sys
import time

import numpy as np


def read_names(path):
    names = {}
    if not path:
        return names
    for line in open(path):
        m = re.match(r"mover (\d+) (.*)", line.strip())
        if m:
            names[int(m.group(1))] = m.group(2)
    return names


def read_init(path, D):
    s = open(path).read()
    m = re.search(r"theta_full\[MM_FEATURES\]\s*=\s*\{([^}]*)\}", s)
    if not m:
        sys.exit(f"no theta_full in {path}")
    v = [float(x.strip().rstrip("f")) for x in m.group(1).split(",")]
    if len(v) != D:
        sys.exit(f"theta_full in {path} has {len(v)} weights, the file has {D} facts")
    return np.array(v, dtype=np.float64)


def chunks(path, chunk_bytes):
    """Yields (X float32 (rows, F), n, played, mover) for a run of whole positions."""
    with open(path, "rb") as f:
        F = struct.unpack("<i", f.read(4))[0]
        buf = b""
        while True:
            more = f.read(chunk_bytes)
            if not more and not buf:
                break
            buf += more
            off = 0
            xs, ns, ks, ms = [], [], [], []
            while True:
                if len(buf) - off < 12:
                    break
                n, k, mover = struct.unpack_from("<iii", buf, off)
                need = 12 + 4 * n * F
                if len(buf) - off < need:
                    break
                xs.append(np.frombuffer(buf, dtype="<f4", count=n * F, offset=off + 12).reshape(n, F))
                ns.append(n); ks.append(k); ms.append(mover)
                off += need
            if xs:
                yield (np.concatenate(xs), np.array(ns, dtype=np.int64), np.array(ks, dtype=np.int64),
                       np.array(ms, dtype=np.int64)), F
            buf = buf[off:]
            if not more:
                if buf:
                    sys.exit(f"{path}: {len(buf)} trailing bytes")
                break


def pass_over(path, theta, free, D, skip, chunk_bytes, want_hessian):
    """One pass: gradient and Hessian over the fitted movers, and test figures for all."""
    g = np.zeros(D); H = np.zeros((D, D))
    per = {}   # mover -> [sum logp, sum log(1/n), top1 hits, positions]
    for (X, n, k, m), F in chunks(path, chunk_bytes):
        Xd = X[:, :D].astype(np.float64)
        P = len(n)
        starts = np.concatenate([[0], np.cumsum(n)[:-1]])
        seg = np.repeat(np.arange(P), n)
        z = Xd @ theta
        zmax = np.maximum.reduceat(z, starts)
        e = np.exp(z - zmax[seg])
        Z = np.add.reduceat(e, starts)
        p = e / Z[seg]
        rows_played = starts + k
        logp = z[rows_played] - zmax - np.log(Z)
        top1 = z[rows_played] >= zmax - 1e-6
        for mv in np.unique(m):
            sel = m == mv
            a = per.setdefault(int(mv), [0.0, 0.0, 0, 0])
            a[0] += float(logp[sel].sum()); a[1] += float(-np.log(n[sel]).sum()); a[2] += int(top1[sel].sum()); a[3] += int(sel.sum())
        if want_hessian:
            fit = m != skip if skip is not None else np.ones(P, dtype=bool)
            fit_rows = fit[seg]
            Xp = Xd[rows_played[fit]]
            pw = (p * fit_rows)[:, None] * Xd
            g += Xp.sum(0) - pw.sum(0)
            Ex = np.add.reduceat(pw, starts, axis=0)
            H += pw.T @ Xd - Ex.T @ Ex
    return g, H, per


def summarise(per, names, skip):
    tot = [0.0, 0.0, 0, 0]
    rows = []
    for mv, a in per.items():
        name = names.get(mv, str(mv))
        rows.append((name, a))
        if mv != skip:
            for i in range(4):
                tot[i] += a[i]
    def fig(a):
        return {"nats_over_uniform": round((a[0] - a[1]) / a[3], 4), "top1": round(a[2] / a[3], 4), "positions": a[3]}
    out = {"all_but_skipped": fig(tot) if tot[3] else None}
    if skip is not None and skip in per:
        out["skipped_mover"] = fig(per[skip])
    rows.sort(key=lambda r: -r[1][3])
    out["per_mover"] = {name: fig(a) for name, a in rows[:30]}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("train"); ap.add_argument("test")
    ap.add_argument("--names", default=None, help="mover names of TRAIN (the feature tool's stderr)")
    ap.add_argument("--names-test", default=None, help="mover names of TEST (default: --names)")
    ap.add_argument("--skip-mover", default="IAmAVirus")
    ap.add_argument("--init", default=None)
    ap.add_argument("--passes", type=int, default=6)
    ap.add_argument("--ridge", type=float, default=1e-2)
    ap.add_argument("--fix", default="2", help="columns held at 0, comma separated")
    ap.add_argument("--cheap", action="store_true")
    ap.add_argument("--chunk-mb", type=int, default=256)
    ap.add_argument("--out", default=None)
    ap.add_argument("--eval-only", action="store_true", help="score --init on the test set and stop")
    ap.add_argument("--index-cols", type=int, default=3,
                    help="trailing columns that are not facts (3 in the piece-square export, 0 in the v35 tool's)")
    ap.add_argument("--columns", type=int, default=None,
                    help="F, the column count of the files; given when TRAIN is a pipe that cannot be peeked")
    a = ap.parse_args()

    F = a.columns if a.columns else np.fromfile(a.train, dtype="<i4", count=1)[0]
    D = int(F) - a.index_cols
    names_tr = read_names(a.names)
    names_te = read_names(a.names_test) if a.names_test else names_tr

    def skip_id(names):
        if not a.skip_mover:
            return None
        ids = [i for i, nm in names.items() if nm == a.skip_mover]
        if not ids:
            print(f"note: mover {a.skip_mover} not in the names file; nothing skipped", file=sys.stderr)
        return ids[0] if ids else None
    skip_tr, skip_te = skip_id(names_tr), skip_id(names_te)
    fixed = set(int(x) for x in a.fix.split(",") if x != "")
    if a.cheap:
        fixed |= set(range(18, D))
    free = np.array([i not in fixed for i in range(D)])
    theta = read_init(a.init, D) if a.init else np.zeros(D)
    theta[~free] = 0.0
    chunk_bytes = a.chunk_mb << 20
    log = {"train": a.train, "test": a.test, "facts": D, "fixed": sorted(fixed), "skip_mover": a.skip_mover, "passes": []}

    t0 = time.time()
    _, _, per = pass_over(a.test, theta, free, D, skip_te, chunk_bytes, False)
    s = summarise(per, names_te, skip_te)
    print(f"initial weights on the test set: {s['all_but_skipped']} ({time.time() - t0:.0f} s)", flush=True)
    log["initial"] = {"theta": theta.round(4).tolist(), "test": s}
    if a.eval_only:
        if a.out:
            json.dump(log, open(a.out, "w"), indent=1)
        return

    for it in range(a.passes):
        t0 = time.time()
        g, H, per_train = pass_over(a.train, theta, free, D, skip_tr, chunk_bytes, True)
        tr = summarise(per_train, names_tr, skip_tr)["all_but_skipped"]
        idx = np.where(free)[0]
        Hf = H[np.ix_(idx, idx)] + a.ridge * np.eye(len(idx))
        step = np.linalg.solve(Hf, g[idx])
        theta_new = theta.copy(); theta_new[idx] += step
        _, _, per = pass_over(a.test, theta_new, free, D, skip_te, chunk_bytes, False)
        s = summarise(per, names_te, skip_te)
        print(f"pass {it + 1}: train {tr}; step max {np.abs(step).max():.4f}; test {s['all_but_skipped']} ({time.time() - t0:.0f} s)", flush=True)
        log["passes"].append({"train": tr, "step_max": float(np.abs(step).max()), "test": s, "theta": theta_new.round(4).tolist()})
        theta = theta_new
        if np.abs(step).max() < 1e-4:
            break

    print("static const float theta[MM_FEATURES] = {" + ", ".join("%.4ff" % x for x in theta) + "};")
    log["final"] = {"theta": theta.round(4).tolist(), "test": s,
                    "c_array": "{" + ", ".join("%.4ff" % x for x in theta) + "}"}
    if a.out:
        json.dump(log, open(a.out, "w"), indent=1)


if __name__ == "__main__":
    main()
