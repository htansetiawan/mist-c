#!/usr/bin/env python3
"""Fit the Fog of War hazard of a position after a move: the chance the mover's
king is captured within four plies, and the chance the mover loses 3+ points of
material within four plies, as logistic models of fog features visible to the
mover. Fitted on all teams' arena games so it is not our own habits.

usage: fit_hazard.py RAW_GAMES_DIR OUT.json [--games 6000] [--min-plies 12]
Needs python-chess and numpy (the iamavirus venv).
"""
import argparse, glob, json, os, sys
import numpy as np
import chess

PIECE = {"P": chess.PAWN, "N": chess.KNIGHT, "B": chess.BISHOP, "R": chess.ROOK, "Q": chess.QUEEN, "K": chess.KING}
VAL = {chess.PAWN: 1, chess.KNIGHT: 3, chess.BISHOP: 3, chess.ROOK: 5, chess.QUEEN: 9}
DIRS = [(1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1)]
KN = [(1, 2), (2, 1), (2, -1), (1, -2), (-1, -2), (-2, -1), (-2, 1), (-1, 2)]
NAMES = ["bias", "L1", "L2", "L3p", "late", "late_L1", "late_L2", "late_L3p", "kingmove", "kingmove_L", "knight_unseen",
         "shelter", "edge", "oppQ_unseen", "opp_heavy10", "capture", "visible_attackers", "diag_lines", "straight_lines"]


def board_from(s64):
    b = chess.Board(None)
    for sq, c in enumerate(s64):
        if c in "-?":
            continue
        b.set_piece_at(sq, chess.Piece(PIECE[c.upper()], c.isupper()))
    return b


def fog_lines(view, k, board, col):
    """open rays from the king whose first empty square is unseen, by kind the opponent still has"""
    them = not col
    has_straight = bool(board.pieces(chess.ROOK, them) or board.pieces(chess.QUEEN, them))
    has_diag = bool(board.pieces(chess.BISHOP, them) or board.pieces(chess.QUEEN, them))
    straight = diag = 0
    for i, (df, dr) in enumerate(DIRS):
        if (i < 4 and not has_straight) or (i >= 4 and not has_diag):
            continue
        f, r = k % 8 + df, k // 8 + dr
        while 0 <= f < 8 and 0 <= r < 8:
            c = view[r * 8 + f]
            if c == "?":
                if i < 4: straight += 1
                else: diag += 1
                break
            if c != "-":
                break
            f += df; r += dr
    return straight, diag


def material(b, col):
    return sum(len(b.pieces(t, col)) * v for t, v in VAL.items())


def features(fr, k, side):
    """features of the position after ply k, from the mover's (side) point of view"""
    col = chess.WHITE if side == "white" else chess.BLACK
    view = fr[k][side]
    b = board_from(fr[k]["board"])
    ks = b.king(col)
    if ks is None:
        return None
    if b.attackers(not col, ks):
        return None   # immediately capturable: the search already scores it as lost
    them = not col
    st, dg = fog_lines(view, ks, b, col)
    L = min(st + dg, 3)
    opp_heavy = sum(len(b.pieces(t, them)) * v for t, v in VAL.items() if t != chess.PAWN)
    late = 1.0 if opp_heavy <= 13 else 0.0
    bp = board_from(fr[k - 1]["board"])
    mv = fr[k]["move"]
    frm, to = chess.parse_square(mv[:2]), chess.parse_square(mv[2:4])
    moved = bp.piece_at(frm)
    kingmove = 1.0 if moved is not None and moved.piece_type == chess.KING else 0.0
    capture = 1.0 if bp.piece_at(to) is not None else 0.0
    knight_unseen = 0
    if b.pieces(chess.KNIGHT, them):
        for df, dr in KN:
            f, r = ks % 8 + df, ks // 8 + dr
            if 0 <= f < 8 and 0 <= r < 8 and view[r * 8 + f] == "?":
                knight_unseen += 1
    shelter = sum(1 for sq in chess.SquareSet(chess.BB_KING_ATTACKS[ks]) if b.piece_at(sq) is not None and b.piece_at(sq).color == col)
    edge = 1.0 if ks % 8 in (0, 7) or ks // 8 in (0, 7) else 0.0
    oq = b.pieces(chess.QUEEN, them)
    oppq_unseen = 1.0 if oq and all(view[s] == "?" for s in oq) else 0.0
    vis_att = sum(1 for sq in chess.SquareSet(chess.BB_ALL) if False)  # placeholder, kept 0: attackers are excluded above
    x = [1.0, float(L == 1), float(L == 2), float(L == 3), late, late * (L == 1), late * (L == 2), late * (L == 3),
         kingmove, kingmove * L, float(knight_unseen), float(shelter), edge, oppq_unseen, opp_heavy / 10.0, capture, float(vis_att), float(dg), float(st)]
    return x


def labels(fr, k, side, loser):
    col = chess.WHITE if side == "white" else chess.BLACK
    end = len(fr) - 1
    # king captured within 4 plies: the referee ends the game with "King capturable" before the
    # capture is played, so the king is still on the final board; use the result instead
    yk = 1.0 if (end - k <= 4 and loser == side) else 0.0
    b0 = board_from(fr[k]["board"])
    bj = board_from(fr[min(k + 4, end)]["board"])
    d0 = material(b0, col) - material(b0, not col)
    d1 = material(bj, col) - material(bj, not col)
    ym = 1.0 if d0 - d1 >= 3 else 0.0
    return yk, ym


def fit_logistic(X, y, ridge=1e-3, iters=30):
    n, d = X.shape
    w = np.zeros(d)
    for _ in range(iters):
        z = X @ w
        p = 1 / (1 + np.exp(-z))
        g = X.T @ (p - y) + ridge * w
        W = p * (1 - p)
        H = (X * W[:, None]).T @ X + ridge * np.eye(d)
        step = np.linalg.solve(H, g)
        w -= step
        if np.abs(step).max() < 1e-6:
            break
    p = 1 / (1 + np.exp(-(X @ w)))
    ll = np.mean(y * np.log(p + 1e-12) + (1 - y) * np.log(1 - p + 1e-12))
    base = y.mean()
    ll0 = base * np.log(base) + (1 - base) * np.log(1 - base)
    return w, ll, ll0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw"); ap.add_argument("out")
    ap.add_argument("--games", type=int, default=6000); ap.add_argument("--min-plies", type=int, default=12)
    args = ap.parse_args()
    dirs = sorted(glob.glob(os.path.join(args.raw, "*")), key=os.path.getmtime, reverse=True)[: args.games]
    X, YK, YM = [], [], []
    used = 0
    for d in dirs:
        try:
            g = json.load(open(os.path.join(d, "replay.json")))
        except Exception:
            continue
        fr = g.get("frames", [])
        if len(fr) < args.min_plies:
            continue
        used += 1
        loser = "white" if g.get("result") == "0-1" else "black" if g.get("result") == "1-0" else None
        if g.get("reason") != "King capturable":
            loser = None
        for k in range(1, len(fr)):
            side = "white" if k % 2 == 1 else "black"
            try:
                x = features(fr, k, side)
            except Exception:
                continue
            if x is None:
                continue
            yk, ym = labels(fr, k, side, loser)
            X.append(x); YK.append(yk); YM.append(ym)
    X = np.array(X); YK = np.array(YK); YM = np.array(YM)
    print(f"{used} games, {len(X)} positions; king captured within 4 plies: {YK.mean()*100:.2f}%; 3+ material lost within 4 plies: {YM.mean()*100:.2f}%")
    out = {"features": NAMES, "n": int(len(X))}
    for name, y in (("king", YK), ("material", YM)):
        w, ll, ll0 = fit_logistic(X, y)
        print(f"\n{name}: mean log-lik {ll:.4f} vs base {ll0:.4f} (gain {ll-ll0:.4f} nats)")
        for nm, c in zip(NAMES, w):
            print(f"  {nm:18s} {c:+.3f}")
        # calibration by decile of predicted probability
        p = 1 / (1 + np.exp(-(X @ w)))
        order = np.argsort(p); bins = np.array_split(order, 10)
        print("  calibration (predicted vs observed, deciles):", " ".join(f"{p[b].mean()*100:.1f}/{y[b].mean()*100:.1f}" for b in bins))
        out[name] = {"coef": [float(c) for c in w], "loglik": float(ll), "base_loglik": float(ll0)}
    json.dump(out, open(args.out, "w"), indent=1)
    print("wrote", args.out)


if __name__ == "__main__":
    sys.exit(main())
