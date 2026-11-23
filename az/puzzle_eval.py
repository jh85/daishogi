#!/usr/bin/env python3
"""How well does a checkpoint's raw net (no search) solve short-mate puzzles?

Usage: puzzle_eval.py CHECKPOINT PUZZLES.tsv
PUZZLES.tsv is tools/puzzlegen output or az/make_puzzles.py's puzzles.tsv
(SFEN, mate length, nodes, legal count, solution moves). Per mate length:
top-1 hit (the net's best legal move is a solution), policy mass on the
solutions, the uniform-guess baseline, median rank of the best solution, and
the value head's win and loss probabilities for the side to move (who wins).
"""
import collections
import subprocess
import sys
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import encode as E  # noqa: E402
import model as M  # noqa: E402

DAISHOGI = str(Path(__file__).resolve().parents[1] / "daishogi")

ckpt, path = sys.argv[1:3]
state = torch.load(ckpt, map_location='cpu', weights_only=True)
net = M.DaiNet(**M.net_config(state)); net.load_state_dict(state['model'] if 'model' in state else state)
net = net.cuda().eval()

def canon(usi, gote):
    return E.flip_usi_move(usi) if gote else usi

stats = collections.defaultdict(lambda: collections.defaultdict(list))
rows = [l.rstrip('\n').split('\t') for l in open(path) if l.strip()]
for i in range(0, len(rows), 64):
    chunk = rows[i:i + 64]
    pos = [E.Position(r[0]) for r in chunk]
    x = torch.from_numpy(np.stack([E.encode_position(p) for p in pos])).cuda()
    with torch.no_grad():
        pol, wdl = net(x)
    pol, wdl = pol.float().cpu(), torch.softmax(wdl.float(), 1).cpu()
    for r, p, lg, v in zip(chunk, pos, pol, wdl):
        L, sol = int(r[1]), set(r[4].split())
        out = subprocess.run([DAISHOGI, 'moves', r[0]], capture_output=True, text=True).stdout.split('\n')[1:]
        legal = [m for m in out if m]
        gote = p.side_to_move == 1
        logits = torch.tensor([lg[E.move_to_index(canon(m, gote))].item() for m in legal])
        prob = torch.softmax(logits, 0)
        best = legal[int(prob.argmax())]
        s = stats[L]
        s['top1'].append(best in sol)
        s['mass'].append(sum(prob[j].item() for j, m in enumerate(legal) if m in sol))
        s['uniform'].append(len(sol) / len(legal))
        s['win'].append(v[0].item()); s['loss'].append(v[2].item())
        ranks = sorted(range(len(legal)), key=lambda j: -prob[j].item())
        s['rank'].append(min(ranks.index(j) for j, m in enumerate(legal) if m in sol) + 1)
print(f"{'len':>3} {'n':>5} {'top1':>6} {'mass':>6} {'uniform':>8} {'med.rank':>8} {'P(win)':>7} {'P(loss)':>7}")
for L in sorted(stats):
    s = stats[L]
    print(f"{L:>3} {len(s['top1']):>5} {np.mean(s['top1']):6.1%} {np.mean(s['mass']):6.1%} "
          f"{np.mean(s['uniform']):8.2%} {np.median(s['rank']):8.0f} {np.mean(s['win']):7.2f} {np.mean(s['loss']):7.2f}")
