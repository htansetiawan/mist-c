"""Attack re-weighting sweep (tools/hazard_calib.c run with MIST_ATTACK_BOOST).
    python3 tools/hazard_reweight.py reweight.tsv [--md OUT.md]
For the plain weights and every (mode, lambda) combo in the file: the true board's weight share and rank in P
over all decisions and over the decisions that lost a piece to an unseen capturer (moved piece taken next reply
and still 3+ net down after our next move, the 30 October definition); the reliability of the belief's estimate
that a played move's destination is attacked from the fog and undefended (u), per bin, fitted and sharp
weighting, with Brier scores; the same for all candidate piece moves and for played moves in plies <= 30; where
the lost decisions fall in the bins of the clean-loss estimate q; and, per mode, the split of the decisions by
whether the true board's feature is above the belief's weighted mean (the boost can only help there), with the
truth's share in each half. Streams the file: nothing is kept per candidate row.
D line layout (31 October follow-up): D game ply |P| capped truth_n, then for the five modes all, und, dest,
cover, destn: a(truth), sharp expectation, fitted expectation; then share and rank for plain and every combo.
"""
import math, sys
from collections import defaultdict

path = sys.argv[1]; md = sys.argv[sys.argv.index('--md') + 1] if '--md' in sys.argv else None
BINS = [(0, 0.01), (0.01, 0.03), (0.03, 0.10), (0.10, 0.30), (0.30, 1.0001)]
MODES = ['all', 'und', 'dest', 'cover', 'destn']
def bin_of(p):
    for i, (lo, hi) in enumerate(BINS):
        if lo <= p < hi: return i
    return len(BINS) - 1

material, replies, dec = {}, {}, {}
with open(path) as f:
    header = f.readline().rstrip('\n').split('\t')
    for line in f:
        if line[0] == 'M':
            p = line.rstrip('\n').split('\t'); material[(p[1], int(p[2]))] = int(p[3])
        elif line[0] == 'R':
            p = line.rstrip('\n').split('\t'); replies[(p[1], int(p[2]))] = (p[3], p[4] == '1')
        elif line[0] == 'D':
            p = line.rstrip('\n').split('\t')
            k = 6 + 3 * len(MODES); vals = p[k:]
            dec[(p[1], int(p[2]))] = {'P': int(p[3]), 'capped': p[4] == '1', 'truth_n': int(p[5]),
                                      'a': {m: float(p[6 + 3 * i]) for i, m in enumerate(MODES)},
                                      'Es': {m: float(p[7 + 3 * i]) for i, m in enumerate(MODES)},
                                      'Ef': {m: float(p[8 + 3 * i]) for i, m in enumerate(MODES)},
                                      'share': [float(vals[2 * i]) for i in range(len(vals) // 2)], 'rank': [int(vals[2 * i + 1]) for i in range(len(vals) // 2)]}
combos = [('plain', 0.0)] + [(n.split('_')[2], float(n.split('_')[3])) for n in header[27::6]]
NC = len(combos)
BASE = {('p', 0): 11, ('p', 1): 12, ('q', 0): 16, ('q', 1): 17, ('u', 0): 23, ('u', 1): 24}
def col(c, w, e):
    if c == 0: return BASE[(e, w)]
    return 27 + 6 * (c - 1) + {'p': 0, 'q': 2, 'u': 4}[e] + w
LABEL = {'p': 14, 'q': 19, 'u': 26}
TABLES = ['played', 'played30', 'all']
acc = {}   # (table, c, w, e) -> [bins of [n, sum_pred, sum_y]], brier [sum_sq, n, sum_y]
for t in TABLES:
    for c in range(NC):
        for w in (0, 1):
            for e in ('p', 'q', 'u'):
                acc[(t, c, w, e)] = ([[0, 0.0, 0] for _ in BINS], [0.0, 0, 0])
lost = set(); lost_bins = defaultdict(lambda: [0] * len(BINS))   # (c, w) -> bins of q among lost decisions
n_played = 0; n_lost_any = 0
with open(path) as f:
    f.readline()
    for line in f:
        if line[0] in 'MRD': continue
        p = line.rstrip('\n').split('\t')
        played = p[7] == '1'; ply = int(p[1])
        labels = {e: int(p[LABEL[e]]) for e in 'pqu'}
        tabs = ['all'] + (['played'] + (['played30'] if ply <= 30 else []) if played else [])
        for t in tabs:
            for c in range(NC):
                for w in (0, 1):
                    for e in 'pqu':
                        x = float(p[col(c, w, e)]); y = labels[e]
                        bins, br = acc[(t, c, w, e)]
                        b = bins[bin_of(x)]; b[0] += 1; b[1] += x; b[2] += y
                        br[0] += (x - y) ** 2; br[1] += 1; br[2] += y
        if played:
            n_played += 1
            g = p[0]; mat = int(p[22])
            rep = replies.get((g, ply + 1))
            taken = bool(rep and rep[0][2:4] == p[3][2:4])
            after2 = material.get((g, ply + 2), material.get((g, ply + 1)))
            loss3 = after2 is not None and after2 - mat <= -3
            if taken and loss3:
                n_lost_any += 1
                if rep[1]:
                    lost.add((g, ply))
                    for c in range(NC):
                        for w in (0, 1):
                            lost_bins[(c, w)][bin_of(float(p[col(c, w, 'q')]))] += 1

out = []
def P(*a): out.append(' '.join(str(x) for x in a))
def name(c): return 'plain' if c == 0 else f"{combos[c][0]} λ={combos[c][1]:g}"
def gm(xs):
    xs = [x for x in xs if x > 0]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else float('nan')
def share_stats(keys, c):
    s = [dec[k]['share'][c] for k in keys if k in dec]; r = [dec[k]['rank'][c] for k in keys if k in dec]
    pres = [x for x in s if x > 0]; rk = sorted(x for x in r if x > 0)
    if not pres: return (len(s), 0, float('nan'), float('nan'), 0)
    return (len(s), len(pres), gm(pres), sorted(pres)[len(pres) // 2], rk[len(rk) // 2])

all_keys = list(dec.keys()); lost_keys = [k for k in lost if k in dec]
P(f"decisions {len(dec)}, played piece moves {n_played}, taken for a 3+ loss {n_lost_any}, by an unseen capturer {len(lost)} (lost decisions with a D line: {len(lost_keys)})")
P(f"true board present in P: {sum(1 for k in all_keys if dec[k]['truth_n'] > 0)} of {len(all_keys)} decisions; among the lost decisions {sum(1 for k in lost_keys if dec[k]['truth_n'] > 0)} of {len(lost_keys)}")
P("\n### The features on the true board against the belief's expectation of them (mean over decisions)\n")
P("| mode | all: a(truth) | sharp E[a] | fitted E[a] | truth above sharp E[a] | lost: a(truth) | sharp E[a] | fitted E[a] | truth above sharp E[a] | above fitted E[a] |"); P("|---|---|---|---|---|---|---|---|---|---|")
for m in MODES:
    ad = [dec[k] for k in all_keys]; ld = [dec[k] for k in lost_keys]
    P(f"| {m} | {sum(d['a'][m] for d in ad)/len(ad):.3f} | {sum(d['Es'][m] for d in ad)/len(ad):.3f} | {sum(d['Ef'][m] for d in ad)/len(ad):.3f} | {sum(1 for d in ad if d['a'][m] > d['Es'][m])} ({sum(1 for d in ad if d['a'][m] > d['Es'][m])/len(ad):.0%}) "
      f"| {sum(d['a'][m] for d in ld)/max(len(ld),1):.3f} | {sum(d['Es'][m] for d in ld)/max(len(ld),1):.3f} | {sum(d['Ef'][m] for d in ld)/max(len(ld),1):.3f} | {sum(1 for d in ld if d['a'][m] > d['Es'][m])} ({sum(1 for d in ld if d['a'][m] > d['Es'][m])/max(len(ld),1):.0%}) | {sum(1 for d in ld if d['a'][m] > d['Ef'][m])} ({sum(1 for d in ld if d['a'][m] > d['Ef'][m])/max(len(ld),1):.0%}) |")

P("\n### The true board's weight in P (weights exp(logw) x exp(lambda a), the search's draw)\n")
P("| weighting | all decisions: geometric mean share | median share | median rank | lost decisions: geometric mean share | median share | median rank |"); P("|---|---|---|---|---|---|---|")
for c in range(NC):
    a = share_stats(all_keys, c); l = share_stats(lost_keys, c)
    P(f"| {name(c)} | {a[2]:.5f} | {a[3]:.5f} | {a[4]} | {l[2]:.5f} | {l[3]:.5f} | {l[4]} |")
P(f"\n(geometric mean and median over the decisions where the true board is in P: {share_stats(all_keys, 0)[1]} of {len(all_keys)}, lost {share_stats(lost_keys, 0)[1]} of {len(lost_keys)}; rank = 1 + the number of worlds weighted above the true board)")

P("\n### The truth's share split by whether its feature is above the belief's sharp expectation (geometric mean; n in the header)\n")
for m in MODES:
    cs = [c for c in range(1, NC) if combos[c][0] == m]
    if not cs: continue
    cm = cs[len(cs) // 2]
    P(f"\n| {m}: decisions | truth above E[a] | " + " | ".join(name(c) for c in [0] + cs) + f" | share up / down / same at {name(cm)} |"); P("|---|---|" + "---|" * (len(cs) + 2))
    for label, keys in (('all', all_keys), ('lost', lost_keys)):
        for above in (True, False):
            sel = [k for k in keys if (dec[k]['a'][m] > dec[k]['Es'][m]) == above]
            up = sum(1 for k in sel if dec[k]['share'][cm] > dec[k]['share'][0] * 1.0001); dn = sum(1 for k in sel if dec[k]['share'][cm] < dec[k]['share'][0] * 0.9999)
            P(f"| {label} | {'yes' if above else 'no'} (n {len(sel)}) | " + " | ".join(f"{gm(dec[k]['share'][c] for k in sel):.5f}" for c in [0] + cs) + f" | {up} / {dn} / {len(sel) - up - dn} |")

def calib(t, e, title):
    P(f"\n### {title}\n")
    P("| weighting | bin | " + " | ".join(f"{lo:.0%}-{min(hi,1):.0%}" for lo, hi in BINS) + " | Brier |"); P("|---|---|" + "---|" * (len(BINS) + 1))
    for c in range(NC):
        for w in (0, 1):
            bins, br = acc[(t, c, w, e)]
            cells = []
            for b in bins:
                if b[0] == 0: cells.append("-"); continue
                mp = b[1] / b[0]; re_ = b[2] / b[0]
                cells.append(f"{mp:.3f}→{re_:.3f} ({re_/mp if mp > 0 else float('nan'):.1f}x, n {b[0]})")
            P(f"| {name(c)} {'fitted' if w == 0 else 'sharp'} | | " + " | ".join(cells) + f" | {br[0]/br[1]:.4f} |")
calib('played', 'u', 'Played moves, event: destination attacked from the fog and undefended (mean predicted → realised, ratio, n)')
calib('played30', 'u', 'Played moves in plies <= 30, the same event')
calib('played', 'p', 'Played moves, event: destination attacked from the fog')
calib('played', 'q', 'Played moves, event: clean one-exchange loss (attacked and undefended, or by a cheaper piece)')
calib('all', 'u', 'All candidate piece moves, event: attacked from the fog and undefended')
P(f"\n### Where the {len(lost)} lost decisions fall in the bins of the clean-loss estimate q of the move played\n")
P("| weighting | " + " | ".join(f"{lo:.0%}-{min(hi,1):.0%}" for lo, hi in BINS) + " |"); P("|---|" + "---|" * len(BINS))
for c in range(NC):
    for w in (0, 1):
        b = lost_bins[(c, w)]
        P(f"| {name(c)} {'fitted' if w == 0 else 'sharp'} | " + " | ".join(f"{x} ({x/max(len(lost),1):.0%})" for x in b) + " |")
text = '\n'.join(out); print(text)
if md: open(md, 'w').write(text + '\n')
