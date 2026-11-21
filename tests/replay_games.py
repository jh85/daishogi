#!/usr/bin/env python3
"""Replays reference-played games through our engine.

For every {"type": "game"} record: every move must be legal for our engine,
the final position must equal the reference's (board, side to move,
lion-capture flag), and the game result must agree (win by royal capture,
no-legal-move loss, repetition draw / perpetual check, or "ongoing" for a
game the generator cut off at its ply limit).

Usage: replay_games.py --binary ./daishogi FILE.jsonl [...]
"""

import argparse
import json
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from refconv import Engine, engine_outcome_token, open_jsonl, state_key  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./daishogi")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    eng = Engine(a.binary)
    n = plies = 0
    fails = []
    outcomes = Counter()
    for path in a.files:
        with open_jsonl(path) as f:
            for line in f:
                r = json.loads(line)
                if r.get("type") != "game":
                    continue
                n += 1
                plies += len(r["moves"])
                if r.get("illegal_at") is not None:
                    # The reference rejects moves[illegal_at]; so must we.
                    k = r["illegal_at"]
                    outcomes["reference rejects a move"] += 1
                    try:
                        eng.set_position(r["start"], r["moves"][:k])
                    except RuntimeError as e:
                        fails.append(f"{r['id']}: prefix rejected: {e}")
                        continue
                    try:
                        eng.set_position(r["start"], r["moves"][:k + 1])
                        fails.append(f"{r['id']}: move {r['moves'][k]} accepted, reference rejects it")
                    except RuntimeError:
                        pass
                    continue
                outcomes[r["outcome"]] += 1
                try:
                    final = eng.set_position(r["start"], r["moves"])
                except RuntimeError as e:
                    fails.append(f"{r['id']}: {e}")
                    continue
                got = engine_outcome_token(eng.status())
                if got != r["outcome"]:
                    fails.append(f"{r['id']}: outcome {got!r}, reference {r['outcome']!r}")
                if r.get("final") and state_key(final) != state_key(r["final"]):
                    fails.append(f"{r['id']}: final position differs\n  ours {final}\n  ref  {r['final']}")
    eng.close()
    print("  reference outcomes: " + ", ".join(f"{k}: {v}" for k, v in sorted(outcomes.items())))
    if fails:
        for msg in fails[:20]:
            print("FAIL " + msg)
        print(f"FAIL: {len(fails)} problems in {n} games")
        return 1
    print(f"OK: {n} reference games ({plies} plies) replayed; all moves legal, "
          "final positions and results identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
