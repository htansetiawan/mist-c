"""Reliability analysis of the belief's fog-hazard estimates (tools/hazard_calib.c output).
    python3 tools/hazard_calib.py calib.tsv [--md OUT.md]
For each estimate (fitted, sharp, uniform): reliability table over bins of predicted probability, Brier score and
log loss against the constant base rate; for all candidate piece moves, the played moves, played moves in plies
<= 30, captures vs non-captures, capped vs exact beliefs; the rank of the played move among the candidates by
each estimate and by truth; and a binned calibration map (predicted -> realised) per estimate.
"""
import csv, math, sys
from collections import defaultdict
path = sys.argv[1]; md = sys.argv[sys.argv.index('--md') + 1] if '--md' in sys.argv else None
rows = []; material = {}; replies = {}
with open(path) as f:
    header = f.readline().rstrip('\n').split('\t')
    for line in f:
        parts = line.rstrip('\n').split('\t')
        if parts[0] == 'M':
            material[(parts[1], int(parts[2]))] = (int(parts[3]), parts[5] if len(parts) > 5 else ''); continue
        if parts[0] == 'R':
            replies[(parts[1], int(parts[2]))] = (parts[3], parts[4] == '1'); continue
        r = dict(zip(header, parts))
        rows.append({'game': r['game'], 'ply': int(r['ply']), 'move': r['move'], 'piece': r['piece'], 'cap_truth': r['capture_truth'] == '1',
                     'cap_seen': r['capture_seen'] == '1', 'played': r['played'] == '1', 'P': int(r['P']), 'capped': r['capped'] == '1',
                     'pf': float(r['p_fitted']), 'ps': float(r['p_sharp']), 'pu': float(r['p_uniform']), 'y': int(r['truth']), 'share': float(r['truth_share']), 'qf': float(r['q_fitted']), 'qs': float(r['q_sharp']), 'qu': float(r['q_uniform']), 'yc': int(r['truth_clean']), 'def': r['defended_truth'] == '1', 'mat': int(r['material_before'])})
EST = [('fitted', 'pf'), ('sharp', 'ps'), ('uniform', 'pu')]
ESTQ = [('fitted', 'qf'), ('sharp', 'qs'), ('uniform', 'qu')]
for r in rows:
    if r['played']:
        after = material.get((r['game'], r['ply'] + 1))
        r['swing'] = (after[0] - r['mat']) if after is not None else None      # our move then their reply
        rep = replies.get((r['game'], r['ply'] + 1)); r['reply'] = rep[0] if rep else ''; r['reply_unseen'] = rep[1] if rep else False
        r['taken'] = 1 if (rep and rep[0][2:4] == r['move'][2:4]) else 0      # the reply captured on our destination
        after2 = material.get((r['game'], r['ply'] + 2))                       # after their reply and our next move (a recapture if any)
        r['swing2'] = (after2[0] - r['mat']) if after2 is not None else r['swing']
        r['loss3'] = 1 if (r['swing2'] is not None and r['swing2'] <= -3) else 0
        r['taken_loss'] = 1 if (r['taken'] and r['loss3']) else 0
        r['taken_loss_unseen'] = 1 if (r['taken_loss'] and r['reply_unseen']) else 0

BINS = [(0, 0.01), (0.01, 0.03), (0.03, 0.10), (0.10, 0.30), (0.30, 1.0001)]
out = []
def P(*a): out.append(' '.join(str(x) for x in a))
def clamp(p): return min(max(p, 1e-6), 1 - 1e-6)
def scores(sel, key):
    n = len(sel); base = sum(r['_y'] for r in sel) / n
    brier = sum((r[key] - r['_y']) ** 2 for r in sel) / n; brier0 = sum((base - r['_y']) ** 2 for r in sel) / n
    ll = -sum(math.log(clamp(r[key])) if r['_y'] else math.log(1 - clamp(r[key])) for r in sel) / n
    ll0 = -(base * math.log(base) + (1 - base) * math.log(1 - base)) if 0 < base < 1 else 0
    return base, brier, brier0, ll, ll0
def table(sel, title, est=None, ykey='y'):
    est = est or EST
    if not sel: return
    for r in sel: r['_y'] = r[ykey]
    base = sum(r['_y'] for r in sel) / len(sel)
    P(f"\n### {title}: {len(sel)} moves, realised rate {base:.3f}\n")
    P("| estimate | bin | n | mean predicted | realised | ratio realised/predicted |"); P("|---|---|---|---|---|---|")
    for name, key in est:
        for lo, hi in BINS:
            s = [r for r in sel if lo <= r[key] < hi]
            if not s: continue
            mp = sum(r[key] for r in s) / len(s); re_ = sum(r['_y'] for r in s) / len(s)
            P(f"| {name} | {lo:.0%}-{min(hi,1):.0%} | {len(s)} | {mp:.3f} | {re_:.3f} | {re_ / mp if mp > 0 else float('nan'):.1f} |")
    P("\n| estimate | Brier | Brier of base rate | log loss | log loss of base rate |"); P("|---|---|---|---|---|")
    for name, key in est:
        b, br, br0, ll, ll0 = scores(sel, key); P(f"| {name} | {br:.4f} | {br0:.4f} | {ll:.4f} | {ll0:.4f} |")
played = [r for r in rows if r['played']]
table(rows, 'All candidate piece moves (N, B, R, Q)')
table(played, 'Moves actually played')
table([r for r in played if r['ply'] <= 30], 'Played moves, plies <= 30')
table([r for r in played if r['cap_truth']], 'Played captures (destination held an enemy piece on the true board)')
table([r for r in played if not r['cap_truth']], 'Played non-captures')
table([r for r in played if r['capped']], 'Played moves with a capped belief')
table([r for r in played if not r['capped']], 'Played moves with an exact belief')
P("\n\n## Clean-loss estimate (attacked from the fog and undefended, or by a cheaper piece): q against the same test on the true board\n")
table(rows, 'All candidate piece moves, clean-loss label', ESTQ, 'yc')
table(played, 'Played moves, clean-loss label', ESTQ, 'yc')
table([r for r in played if r['ply'] <= 30], 'Played moves, plies <= 30, clean-loss label', ESTQ, 'yc')
sw = [r for r in played if r.get('swing') is not None]
P("\n\n## Realised outcome: our piece move, then the opponent's reply (material swing within one exchange)\n")
table(sw, 'Played moves, label = the reply captured the piece we moved', EST, 'taken')
table(sw, 'Played moves, label = moved piece taken and still 3+ net down after our next move', ESTQ, 'taken_loss')
table(sw, 'Played moves, label = the same, by a capturer that was UNSEEN at our decision', ESTQ, 'taken_loss_unseen')
table([r for r in sw if r['ply'] <= 30], 'Played moves, plies <= 30, label = taken by an unseen capturer, 3+ net down', ESTQ, 'taken_loss_unseen')
lost = [r for r in sw if r['taken_loss_unseen']]
P(f"\n### What the guards would have seen on the {len(lost)} played piece moves taken by an unseen capturer for a 3+ net loss ({len(lost)/max(len(sw),1):.3f} of {len(sw)} played piece moves; {sum(1 for r in sw if r['taken_loss'])} taken for a loss by any capturer)\n")
P("| estimate | < 1% | 1-3% | 3-10% | 10-30% | >= 30% |"); P("|---|---|---|---|---|---|")
for name, key in ESTQ:
    c = [sum(1 for r in lost if lo <= r[key] < hi) for lo, hi in BINS]
    P(f"| {name} | " + ' | '.join(f"{x} ({x/len(lost):.0%})" for x in c) + " |")
P(f"- of these, the true board says clean-loss (yc) on {sum(r['yc'] for r in lost)}; attacked-from-fog on {sum(r['y'] for r in lost)}; captures on {sum(r['cap_truth'] for r in lost)}; belief capped on {sum(r['capped'] for r in lost)}; median truth share {sorted(r['share'] for r in lost)[len(lost)//2] if lost else None:.4g}")
# base rates by ply bucket, played moves
P("\n### Realised fog-attacked rate of played piece moves by ply\n"); P("| plies | n | rate | mean fitted | mean sharp | mean uniform |"); P("|---|---|---|---|---|---|")
for lo, hi in ((1, 10), (11, 20), (21, 30), (31, 40), (41, 60), (61, 400)):
    s = [r for r in played if lo <= r['ply'] <= hi]
    if s: P(f"| {lo}-{hi} | {len(s)} | {sum(r['y'] for r in s)/len(s):.3f} | {sum(r['pf'] for r in s)/len(s):.3f} | {sum(r['ps'] for r in s)/len(s):.3f} | {sum(r['pu'] for r in s)/len(s):.3f} |")
# rank of the played move among candidates
dec = defaultdict(list)
for r in rows: dec[(r['game'], r['ply'])].append(r)
P("\n### The played move among the candidates (decisions with a played piece move and >= 2 candidates)\n")
cnt = 0; stats = defaultdict(float)
for k, cands in dec.items():
    pl = [r for r in cands if r['played']]
    if not pl or len(cands) < 2: continue
    cnt += 1; p = pl[0]
    for name, key in EST:
        stats[name + ' played is the riskiest'] += p[key] >= max(r[key] for r in cands)
        stats[name + ' played above median risk'] += p[key] > sorted(r[key] for r in cands)[len(cands) // 2]
    stats['played was truly fog-attacked'] += p['y']
    stats['some candidate was truly fog-attacked'] += any(r['y'] for r in cands)
    stats['played attacked while a safe candidate existed'] += p['y'] and any(not r['y'] for r in cands)
P(f"decisions {cnt}"); [P(f"- {k}: {v/cnt:.3f}") for k, v in stats.items()]
# calibration map proposal: binned mapping on played moves (what the guards would see)
P("\n### Binned calibration map on played moves (predicted -> realised), the input a guard would use\n")
for name, key in EST:
    s = played; pieces = []
    for lo, hi in [(0, 0.001), (0.001, 0.01), (0.01, 0.03), (0.03, 0.1), (0.1, 0.3), (0.3, 1.0001)]:
        b = [r for r in s if lo <= r[key] < hi]
        if b: pieces.append(f"{lo:g}-{min(hi,1):g}: {sum(r['y'] for r in b)/len(b):.3f} (n {len(b)})")
    P(f"- {name}: " + '; '.join(pieces))
text = '\n'.join(out); print(text)
if md: open(md, 'w').write(text + '\n')
