#!/usr/bin/env python3
"""Per-iteration summary of a run_loop.py run: game count, length
(mean/median/max), endings, results, the last training loss, and the
strength-tournament results so far.

Usage: run_status.py [RUN_DIR] [--last N]
"""

import argparse
import json
import re
import statistics
import sys
from collections import Counter
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir", nargs="?", default="az/data/run1")
    ap.add_argument("--last", type=int, default=0, help="only the last N iterations")
    a = ap.parse_args()
    run = Path(a.run_dir)
    iters = sorted((run / "games").glob("iter_*"))
    if a.last:
        iters = iters[-a.last:]
    print("iter   games  mean  median  max  endings                         S/D/G    loss (p, v)")
    for d in iters:
        it = int(d.name.split("_")[1])
        lengths, reasons, results = [], Counter(), Counter()
        for f in d.rglob("game_*.json"):
            try:
                rec = json.loads(f.read_text())
            except (json.JSONDecodeError, OSError):
                continue
            lengths.append(len(rec["moves"]))
            reasons[rec["reason"]] += 1
            results[rec["result"]] += 1
        if not lengths:
            continue
        loss = ""
        log = run / f"train_{it:06d}.log"
        if log.exists():
            m = [x for x in log.read_text().splitlines() if " loss " in x and "val" not in x]
            if m:
                g = re.search(r"loss ([\d.]+) \(p ([\d.]+) v ([\d.]+)\)", m[-1])
                if g:
                    loss = f"{g.group(1)} ({g.group(2)}, {g.group(3)})"
        ends = " ".join(f"{k}:{v}" for k, v in reasons.most_common())
        sdg = f"{results[1]}/{results[0]}/{results[-1]}"
        print(f"{it:4d}  {len(lengths):6d}  {statistics.mean(lengths):4.0f}  "
              f"{statistics.median(lengths):6.0f}  {max(lengths):4d}  {ends:30s}  {sdg:7s}  {loss}")
    for summary in sorted((run / "strength").glob("iter_*/summary.json")):
        print(f"\nstrength {summary.parent.name}:")
        for row in json.loads(summary.read_text()):
            print("  " + json.dumps(row))
    return 0


if __name__ == "__main__":
    sys.exit(main())
