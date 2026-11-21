#!/usr/bin/env python3
"""MCTS sanity test through the CLI. Ported from ../chushogi
(tests/az_mcts_test.py, commit d578c81; a regression test for a
backup-perspective bug there).

On a mate-in-one position (the rook takes the lone king), MCTS with the
uniform evaluator must choose the capture: the terminal edge must outrank
every non-terminal alternative. Run for both colours (the Gote case goes
through the 180-degree flip) and two root temperatures.
"""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Sente: rook 8m takes king 8c. Gote: the same position rotated 180 degrees.
SENTE_M1 = "15/6go8/7ki7/15/15/15/15/15/15/15/15/15/7RO7/15/KI14 b - 1"
GOTE_M1 = "14ki/15/7ro7/15/15/15/15/15/15/15/15/15/7KI7/8GO6/15 w - 1"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--az", default=str(ROOT / "daishogi-az"))
    a = ap.parse_args()
    az = subprocess.Popen([a.az], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def cmd(line):
        az.stdin.write(line + "\n")
        az.stdin.flush()
        return az.stdout.readline().strip()

    total = failures = 0
    for temperature in (1.0, 1.25):
        for sfen, want in ((SENTE_M1, "8m8c"), (GOTE_M1, "8c8m")):
            assert cmd(f"position sfen {sfen}") == "ok"
            out = cmd(f"go az --sims 64 --batch 8 --eval random --root-policy-temp {temperature}")
            total += 1
            if out != f"bestmove {want}":
                failures += 1
                print(f"FAIL: T={temperature}, {sfen} -> {out}, want bestmove {want}")
    az.stdin.write("quit\n")
    az.stdin.flush()
    az.wait(timeout=10)
    print(f"{total - failures}/{total} az mcts checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
