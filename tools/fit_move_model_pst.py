"""Fit the opponent move model with piece-square tables: a conditional logit over the
mover's legal moves, P(m | s) = softmax(theta . phi(s, m) + pst_from[piece][from] +
pst_to[piece][to]), by Adam on the mean log-likelihood.

    python3 tools/fit_move_model_pst.py TRAIN.bin TEST.bin [--out src/movemodel_theta.h]
        [--epochs 8] [--positions N] [--threads 4] [--l2 1e-4] [--lr 0.05]
        [--summary fit.json] [--movemodel build/move_features --test-txt TEST.txt]

Reads tools/move_features.c output (F = MM_FEATURES + 3: the dense features, then the
moved piece type and the mirrored from and to squares). Fits the full model and the cheap
one (attack features zeroed), each without and with the tables; reports nats per move
over uniform and top-1 on the test set, per mover for the final full model, and writes
the C header movemodel.c includes. The compiled model before the fit (the header's
theta_full) is scored on the same test set first.
"""
import argparse
import json
import math
import re
import struct
import subprocess
import sys
import time

import numpy as np
import torch

NAMES = ["capture", "captured_value", "king_capture", "knight", "bishop", "rook", "queen", "king",
         "double_push", "castle", "promote_queen", "underpromote", "forward", "retreat", "centre",
         "develop", "recapture", "advanced_pawn", "check", "escape", "into_attack", "hang_value",
         "self_capturable", "undefended", "king_zone", "xray_king", "check_unseen", "threat_unseen",
         "obs_escape_seen", "obs_into_attack_seen", "obs_hang_seen", "obs_capture_defended_seen",
         "obs_self_capturable_seen", "obs_threat_seen", "obs_approach_seen", "obs_reveal",
         "obs_threat_seen_value", "obs_capture_defended_unseen", "obs_fog_exposure"]
DENSE = len(NAMES)
ATTACK = slice(18, DENSE)   # attack and observation features: zero in the cheap model
OBS = slice(28, DENSE)      # the observation features: zero in the control fit
PIECES = 6


def load(path, limit=None):
    """Flat rows: X (M, DENSE) float32, idx (M, 3) int64, per position starts/counts/played/mover."""
    data = np.fromfile(path, dtype=np.uint8)
    F = int(np.frombuffer(data[:4].tobytes(), dtype="<i4")[0])
    assert F == DENSE + 3, f"expected {DENSE + 3} columns, file has {F}"
    off = 4
    xs, played, counts, movers = [], [], [], []
    mv = memoryview(data)
    while off < len(data) and (limit is None or len(counts) < limit):
        n, k, mover = struct.unpack_from("<iii", mv, off)
        off += 12
        xs.append(np.frombuffer(mv, dtype="<f4", count=n * F, offset=off).reshape(n, F))
        off += 4 * n * F
        played.append(k); counts.append(n); movers.append(mover)
    A = np.concatenate(xs)
    X = np.ascontiguousarray(A[:, :DENSE])
    idx = A[:, DENSE:].astype(np.int64)
    counts = np.array(counts, dtype=np.int64)
    starts = np.concatenate([[0], np.cumsum(counts)])
    return X, idx, starts, counts, np.array(played, dtype=np.int64), np.array(movers, dtype=np.int64)


class Data:
    def __init__(self, path, limit, cheap, zero=None):
        X, idx, starts, counts, played, movers = load(path, limit)
        if cheap:
            X[:, ATTACK] = 0
        if zero is not None:
            X[:, zero] = 0
        self.X = torch.from_numpy(X)
        self.idx = torch.from_numpy(idx[:, 0] * 64 + idx[:, 1]), torch.from_numpy(idx[:, 0] * 64 + idx[:, 2])
        self.starts = torch.from_numpy(starts)
        self.counts = torch.from_numpy(counts)
        self.played = torch.from_numpy(played)
        self.movers = torch.from_numpy(movers)
        self.N = len(counts)
        self.seg = torch.repeat_interleave(torch.arange(self.N), self.counts)


def logits(d, rows, theta, pf, pt):
    z = d.X[rows] @ theta
    if pf is not None:
        z = z + pf[d.idx[0][rows]] + pt[d.idx[1][rows]]
    return z


def batch_stats(d, a, b, theta, pf, pt):
    """log P(played) and top-1 hits for positions [a, b)."""
    r0, r1 = int(d.starts[a]), int(d.starts[b])
    rows = slice(r0, r1)
    z = logits(d, rows, theta, pf, pt)
    seg = d.seg[rows] - a
    B = b - a
    zmax = torch.full((B,), -1e30).scatter_reduce(0, seg, z, "amax")
    e = torch.exp(z - zmax[seg])
    Z = torch.zeros(B).index_add(0, seg, e)
    zp = z[d.starts[a:b] + d.played[a:b] - r0]
    logp = zp - zmax - torch.log(Z)
    top1 = zp >= zmax - 1e-6
    return logp, top1


def evaluate(d, theta, pf, pt, step=8192):
    logps, tops = [], []
    with torch.no_grad():
        for a in range(0, d.N, step):
            lp, t1 = batch_stats(d, a, min(a + step, d.N), theta, pf, pt)
            logps.append(lp); tops.append(t1)
    logp = torch.cat(logps); top1 = torch.cat(tops)
    gain = (logp + torch.log(d.counts.float())).mean().item()
    return {"nats_over_uniform": round(gain, 4), "top1": round(top1.float().mean().item(), 4),
            "nll": round(-logp.mean().item(), 4), "positions": d.N}, logp, top1


def fit(train, test, use_pst, epochs, lr, l2, batch, log):
    theta = torch.zeros(DENSE, requires_grad=True)
    pf = torch.zeros(PIECES * 64, requires_grad=True) if use_pst else None
    pt = torch.zeros(PIECES * 64, requires_grad=True) if use_pst else None
    params = [theta] + ([pf, pt] if use_pst else [])
    opt = torch.optim.Adam(params, lr=lr)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=epochs * math.ceil(train.N / batch))
    g = torch.Generator().manual_seed(1)
    for ep in range(epochs):
        t0 = time.time()
        order = torch.randperm(math.ceil(train.N / batch), generator=g)
        tot = 0.0
        for j in order.tolist():
            a, b = j * batch, min((j + 1) * batch, train.N)
            logp, _ = batch_stats(train, a, b, theta, pf, pt)
            loss = -logp.mean()
            if use_pst:
                loss = loss + l2 * (pf.pow(2).sum() + pt.pow(2).sum())
            opt.zero_grad(); loss.backward(); opt.step(); sched.step()
            tot += -logp.mean().item() * (b - a)
        te = evaluate(test, theta, pf, pt)[0]
        log(f"  epoch {ep + 1}: train nll {tot / train.N:.4f}; test {te['nats_over_uniform']:+.4f} nats over uniform, top-1 {te['top1']:.4f} ({time.time() - t0:.0f} s)")
    return theta.detach(), (pf.detach() if use_pst else None), (pt.detach() if use_pst else None)


def c_array(name, v, fmt="%.4ff"):
    return f"static const float {name}[MM_FEATURES] = {{" + ", ".join(fmt % x for x in v.tolist()) + "};\n"


def c_table(name, v):
    rows = v.reshape(PIECES, 64).tolist()
    body = ",\n".join("    {" + ", ".join("%.4ff" % x for x in r) + "}" for r in rows)
    return f"static const float {name}[6][64] = {{\n{body}\n}};\n"


def header(full, cheap, note):
    return f'''/* mist-c: fitted opponent move model parameters. GENERATED by
 * tools/fit_move_model_pst.py; do not edit by hand. theta_* weight the
 * features of movemodel.h; pst_from_*[piece][square] and pst_to_* are the
 * piece-square terms of the moved piece's origin and destination, squares
 * mirrored for Black so one table serves both colours.
 * {note} */
#ifndef FOW_MOVEMODEL_THETA_H
#define FOW_MOVEMODEL_THETA_H

{c_array("theta_full", full[0])}{c_array("theta_cheap", cheap[0])}{c_table("pst_from_full", full[1])}{c_table("pst_to_full", full[2])}{c_table("pst_from_cheap", cheap[1])}{c_table("pst_to_cheap", cheap[2])}
#endif
'''


def mover_names(binary, txt):
    if not binary or not txt:
        return {}
    r = subprocess.run([binary, txt, "/dev/null", "1"], capture_output=True, text=True)
    names = {}
    for line in r.stderr.splitlines():
        m = re.match(r"mover (\d+) (.*)", line)
        if m:
            names[int(m.group(1))] = m.group(2)
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("train"); ap.add_argument("test")
    ap.add_argument("--out", default=None)
    ap.add_argument("--epochs", type=int, default=8)
    ap.add_argument("--positions", type=int, default=None)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--l2", type=float, default=1e-4)
    ap.add_argument("--lr", type=float, default=0.05)
    ap.add_argument("--batch", type=int, default=2048)
    ap.add_argument("--summary", default=None)
    ap.add_argument("--movemodel", default="build/move_features")
    ap.add_argument("--test-txt", default=None)
    ap.add_argument("--current", default="src/movemodel_theta.h", help="header whose theta_full is scored first")
    ap.add_argument("--note", default="")
    ap.add_argument("--no-dense-refit", action="store_true", help="skip the dense-only refit")
    ap.add_argument("--control", action="store_true", help="also fit the full+PST model with the observation features zeroed, as the control")
    ap.add_argument("--out-control", default=None, help="write the control fit (its cheap part reuses the main cheap fit) as a header too")
    args = ap.parse_args()
    torch.set_num_threads(args.threads)
    log = lambda s: print(s, flush=True)
    summary = {"args": vars(args)}

    t0 = time.time()
    train_full = Data(args.train, args.positions, cheap=False)
    test_full = Data(args.test, None, cheap=False)
    log(f"loaded {train_full.N} train and {test_full.N} test positions ({train_full.X.shape[0]} and {test_full.X.shape[0]} moves) in {time.time() - t0:.0f} s")

    # the compiled model before this fit, dense features only
    try:
        text = open(args.current).read()
        cur = torch.tensor([float(x) for x in re.search(r"theta_full\[MM_FEATURES\] = \{(.*?)\}", text, re.S).group(1).replace("f", "").split(",")])
        cur = torch.cat([cur, torch.zeros(DENSE - len(cur))])  # a header written before new features were added
        pf_cur = pt_cur = None
        m = re.search(r"pst_from_full\[6\]\[64\] = \{(.*?)\n\};", text, re.S)
        if m:
            vals = [float(x) for x in re.findall(r"-?\d+\.\d+", m.group(1))]
            if any(vals):
                pf_cur = torch.tensor(vals)
                pt_cur = torch.tensor([float(x) for x in re.findall(r"-?\d+\.\d+", re.search(r"pst_to_full\[6\]\[64\] = \{(.*?)\n\};", text, re.S).group(1))])
        summary["current_full"] = evaluate(test_full, cur, pf_cur, pt_cur)[0]
        log(f"current compiled full model on the test set: {summary['current_full']}")
    except Exception as e:  # noqa: BLE001
        log(f"current model not scored: {e}")

    names = mover_names(args.movemodel, args.test_txt)

    def per_mover(d, logp, top1):
        per = {}
        for mid in sorted(set(d.movers.tolist())):
            sel = d.movers == mid
            per[names.get(mid, str(mid))] = {"positions": int(sel.sum()),
                                             "nats_over_uniform": round((logp[sel] + torch.log(d.counts[sel].float())).mean().item(), 4),
                                             "top1": round(top1[sel].float().mean().item(), 4)}
        return per

    if not args.no_dense_refit:
        log("full model, dense features only (refit):")
        th, _, _ = fit(train_full, test_full, False, args.epochs, args.lr, args.l2, args.batch, log)
        summary["refit_dense_full"] = evaluate(test_full, th, None, None)[0]
    if args.control:
        log("control: full model with piece-square tables, observation features zeroed:")
        train_ctl = Data(args.train, args.positions, cheap=False, zero=OBS)
        test_ctl = Data(args.test, None, cheap=False, zero=OBS)
        th_0, pf_0, pt_0 = fit(train_ctl, test_ctl, True, args.epochs, args.lr, args.l2, args.batch, log)
        summary["fit_pst_full_control"], logp, top1 = evaluate(test_ctl, th_0, pf_0, pt_0)
        summary["per_mover_full_pst_control"] = per_mover(test_ctl, logp, top1)
        summary["theta_full_control"] = dict(zip(NAMES, [round(x, 4) for x in th_0.tolist()]))
        del train_ctl, test_ctl
    log("full model with piece-square tables:")
    th_f, pf_f, pt_f = fit(train_full, test_full, True, args.epochs, args.lr, args.l2, args.batch, log)
    summary["fit_pst_full"], logp, top1 = evaluate(test_full, th_f, pf_f, pt_f)
    summary["per_mover_full_pst"] = per_mover(test_full, logp, top1)
    summary["theta_full"] = dict(zip(NAMES, [round(x, 4) for x in th_f.tolist()]))
    del train_full
    train_cheap = Data(args.train, args.positions, cheap=True)
    test_cheap = Data(args.test, None, cheap=True)
    log("cheap model with piece-square tables:")
    th_c, pf_c, pt_c = fit(train_cheap, test_cheap, True, args.epochs, args.lr, args.l2, args.batch, log)
    summary["fit_pst_cheap"] = evaluate(test_cheap, th_c, pf_c, pt_c)[0]
    th_c[ATTACK] = 0
    if args.out:
        open(args.out, "w").write(header((th_f, pf_f, pt_f), (th_c, pf_c, pt_c), args.note))
        log(f"wrote {args.out}")
    if args.control and args.out_control:
        open(args.out_control, "w").write(header((th_0, pf_0, pt_0), (th_c, pf_c, pt_c), args.note + " (control: observation features zeroed)"))
        log(f"wrote {args.out_control}")
    if args.summary:
        json.dump(summary, open(args.summary, "w"), indent=1)
    log(json.dumps({k: v for k, v in summary.items() if k.startswith(("current", "refit", "fit"))}, indent=1))


if __name__ == "__main__":
    main()
