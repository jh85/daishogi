"""Capture audit of self-play games: is the agent passing up free material?

Replays game records through tools/capture_audit (a capture-only search at
every position; see that file) and reports, by game phase:

  contact     legal captures per position
  chances     positions where the side to move can win material now
  taken       how often it then plays a capture that wins it
  visits      in full searches (recorded samples), the share of visits on
              the winning captures, and how often one is the most visited
  hanging     positions where the mover's own material hangs, and how often
              it still hangs after the move

Usage:
  capture_audit.py GAMES_DIR [GAMES_DIR ...] [--workers 8]
  capture_audit.py --random 200 [--workers 8]     # uniformly random play
"""
import argparse
import collections
import json
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'az'))
import encode as E  # noqa: E402

WIN = 100000
PHASES = [(0, 250), (250, 500), (500, 1000), (1000, 1501)]
THRESHOLDS = (40, 150, 300)  # a pawn, a gold/lance, a rook


def run_tool(lines, args):
    cmd = [str(ROOT / 'tools/capture_audit'), '--depth', str(args.depth)]
    out = subprocess.run(cmd, input=''.join(lines), capture_output=True, text=True, check=True)
    return out.stdout


def parse(text):
    """-> {game id: {ply: row}}"""
    games = collections.defaultdict(dict)
    for line in text.splitlines():
        f = line.split()
        caps = {}
        for c in f[8:]:
            usi, g = c.rsplit(':', 1)
            caps[usi] = int(g)
        games[f[0]][int(f[1])] = dict(
            stm=int(f[2]), ncap=int(f[3]), gain=int(f[4]), threat=int(f[5]),
            played=f[6], pgain=None if f[7] == '-' else int(f[7]), caps=caps)
    return games


def phase_of(ply):
    for i, (a, b) in enumerate(PHASES):
        if a <= ply < b:
            return i
    return len(PHASES) - 1


def summarize(games, samples=None):
    S = [collections.Counter() for _ in PHASES]
    for gid, rows in games.items():
        for ply, r in rows.items():
            s = S[phase_of(ply)]
            s['positions'] += 1
            s['ncap'] += r['ncap']
            s['contact'] += r['ncap'] > 0
            if r['gain'] >= WIN:
                s['win_avail'] += 1
                s['win_taken'] += r['pgain'] is not None and r['pgain'] >= WIN
            for t in THRESHOLDS:
                if r['gain'] >= t:
                    s[f'chance{t}'] += 1
                    s[f'took_some{t}'] += r['pgain'] is not None and r['pgain'] >= t
                    s[f'took_best{t}'] += r['pgain'] is not None and r['pgain'] >= r['gain']
                if r['threat'] >= t and r['played'] != '-':
                    nxt = rows.get(ply + 1)
                    if nxt is not None:
                        s[f'hang{t}'] += 1
                        # still hanging = the opponent can now win >= t
                        s[f'still{t}'] += nxt['gain'] >= t
            if samples is not None and (gid, ply) in samples and r['gain'] >= 40:
                pol = samples[(gid, ply)]
                good = {E.move_to_index(E.flip_usi_move(u) if r['stm'] else u)
                        for u, g in r['caps'].items() if g >= 40}
                s['vis_n'] += 1
                s['vis_good'] += sum(p for i, p in pol if i in good)
                s['vis_top'] += max(pol, key=lambda x: x[1])[0] in good if pol else 0
    return S


def report(title, S):
    print(f"\n== {title}")
    hdr = f"{'plies':>11} {'positions':>9} {'caps/pos':>8} {'contact':>7}"
    for t in THRESHOLDS:
        hdr += f" {'win>=' + str(t):>8} {'took':>5} {'best':>5}"
    print(hdr)
    for (a, b), s in zip(PHASES, S):
        n = s['positions']
        if not n:
            continue
        row = f"{f'{a}-{b - 1}':>11} {n:>9} {s['ncap'] / n:>8.2f} {s['contact'] / n:>7.1%}"
        for t in THRESHOLDS:
            c = s[f'chance{t}']
            row += f" {c / n:>8.1%} {s[f'took_some{t}'] / max(c, 1):>5.0%} {s[f'took_best{t}'] / max(c, 1):>5.0%}"
        print(row)
    tot = sum(S, collections.Counter())
    if tot['win_avail']:
        print(f"royal capture available (wins the game): {tot['win_avail']} positions, "
              f"played {tot['win_taken'] / tot['win_avail']:.0%}")
    for t in THRESHOLDS:
        if tot[f'hang{t}']:
            print(f"own material >= {t} hanging: {tot[f'hang{t}'] / tot['positions']:.1%} of positions; "
                  f"still hanging after the move {tot[f'still{t}'] / tot[f'hang{t}']:.0%}")
    if tot['vis_n']:
        print(f"full searches with material >= 40 to win ({tot['vis_n']}): visit share on winning "
              f"captures {tot['vis_good'] / tot['vis_n']:.0%}; most visited move is one "
              f"{tot['vis_top'] / tot['vis_n']:.0%}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dirs', nargs='*')
    ap.add_argument('--random', type=int, default=0, help='audit N uniformly random games instead')
    ap.add_argument('--workers', type=int, default=8)
    ap.add_argument('--depth', type=int, default=4)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    if bool(a.dirs) == bool(a.random):
        ap.error('give game directories or --random N')
    if a.random:
        cmds = [[str(ROOT / 'tools/capture_audit'), '--depth', str(a.depth), '--random',
                 str(len(range(w, a.random, a.workers))), '--seed', str(a.seed * 1000 + w)]
                for w in range(a.workers)]
        with ThreadPoolExecutor(a.workers) as ex:
            outs = list(ex.map(lambda c: subprocess.run(c, capture_output=True, text=True,
                                                        check=True).stdout, cmds))
        games = {}
        for w, o in enumerate(outs):  # ids repeat across workers
            for gid, rows in parse(o).items():
                games[f'{w}_{gid}'] = rows
        report(f'uniformly random play, {len(games)} games', summarize(games))
        return
    files = sorted(f for d in a.dirs for f in Path(d).rglob('game_*.json'))
    lines, samples = [], {}
    for k, f in enumerate(files):
        r = json.loads(f.read_text())
        gid = f'g{k}'
        lines.append(f"{gid}\t{r['initial']}\t{' '.join(r['moves'])}\n")
        for s in r['samples']:
            samples[(gid, s['ply'])] = s['policy']
    chunks = [lines[w::a.workers] for w in range(a.workers)]
    with ThreadPoolExecutor(a.workers) as ex:
        outs = list(ex.map(lambda c: run_tool(c, a), chunks))
    games = {}
    for o in outs:
        games.update(parse(o))
    report(f'{len(files)} self-play games from {", ".join(a.dirs)}', summarize(games, samples))


if __name__ == '__main__':
    main()
