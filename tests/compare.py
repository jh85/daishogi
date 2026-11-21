#!/usr/bin/env python3
"""Compares our legal move generator with the reference corpus.

For every record with an "expected" move list (reference moves converted to
our USI notation), the engine's complete legal-move set must be identical
(the moves our rules deliberately forbid or add are excluded from / included
in "expected", and listed as "reference_only" / "ours_only"):
no missing move, no extra move. Where the record has them, the position's
result ("outcome", e.g. "ongoing" or "win gote" for a side without legal
moves or royals) and the position reached by each listed move ("next",
board + side to move + lion-capture flag) must match as well. Mismatches
are printed and written to
tests/mismatches.jsonl with the position, expected and actual moves, so each
one can be reproduced with

    ./daishogi moves "<sfen>"

Usage: compare.py --binary ./daishogi FILE.jsonl [...]
"""

import argparse
import json
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from refconv import Engine, engine_outcome_token, open_jsonl, state_hash  # noqa: E402


def coverage(stats, sfen, moves):
    """Rule-feature counters, to show what the corpus exercises."""
    f = sfen.split()
    if f[2] != "-":
        stats["positions with lion-capture restriction (先獅子)"] += 1
    for m in moves:
        squares = sum(1 for ch in m if ch.isalpha())
        if squares == 3:
            stats["two-step (lion-power) moves"] += 1
        if m.endswith("+"):
            stats["promotion moves"] += 1
    if not moves:
        stats["positions without legal moves"] += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./daishogi")
    ap.add_argument("--mismatches", default=os.path.join(os.path.dirname(__file__), "mismatches.jsonl"))
    ap.add_argument("--max-report", type=int, default=10)
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()

    eng = Engine(a.binary)
    n_pos = n_moves = n_trans = 0
    bad = []
    per_file = Counter()
    stats = Counter()
    for path in a.files:
        with open_jsonl(path) as f:
            for line in f:
                r = json.loads(line)
                if "expected" not in r:
                    continue
                trans, outcome = {}, None
                try:
                    if r.get("next"):
                        trans = eng.transitions(r["sfen"])
                        got = list(trans)
                    else:
                        got = eng.legal(r["sfen"])
                    outcome = engine_outcome_token(eng.status())
                except RuntimeError as e:
                    got = [f"<engine error: {e}>"]
                exp = r["expected"]
                n_trans += len(r.get("next", {}))
                wrong_next = sorted(u for u, h in r.get("next", {}).items()
                                    if u in trans and state_hash(trans[u]) != h)
                if wrong_next or ("outcome" in r and outcome != r["outcome"]):
                    bad.append({"id": r["id"], "file": os.path.basename(path), "sfen": r["sfen"],
                                "outcome": outcome, "expected_outcome": r.get("outcome"),
                                "wrong_transitions": {u: trans[u] for u in wrong_next},
                                "missing": [], "extra": []})
                n_pos += 1
                n_moves += len(exp)
                per_file[os.path.basename(path)] += 1
                coverage(stats, r["sfen"], exp)
                # Deliberate differences (lishogi / JCSA rules, docs/RULES.md);
                # "expected" already omits / includes them.
                if r.get("reference_only"):
                    stats["reference-only moves (we forbid them)"] += len(r["reference_only"])
                if r.get("ours_only"):
                    stats["ours-only moves (the reference forbids them)"] += len(r["ours_only"])
                if sorted(got) != sorted(exp):
                    gs, es = set(got), set(exp)
                    bad.append({"id": r["id"], "file": os.path.basename(path), "sfen": r["sfen"],
                                "missing": sorted(es - gs), "extra": sorted(gs - es),
                                "duplicates": len(got) != len(gs),
                                "expected": exp, "actual": sorted(got)})
    eng.close()

    for f, n in sorted(per_file.items()):
        print(f"  {f}: {n} positions")
    for k, v in sorted(stats.items()):
        print(f"  coverage: {k}: {v}")
    if os.path.exists(a.mismatches):
        os.remove(a.mismatches)
    if bad:
        with open(a.mismatches, "w") as out:
            for b in bad:
                out.write(json.dumps(b) + "\n")
        for b in bad[:a.max_report]:
            print(f"MISMATCH {b['id']} ({b['file']})\n  sfen    {b['sfen']}\n"
                  f"  missing {' '.join(b['missing']) or '-'}\n  extra   {' '.join(b['extra']) or '-'}")
            if b.get("wrong_transitions"):
                for u, s in b["wrong_transitions"].items():
                    print(f"  after {u}: ours {s} differs from the reference")
            if b.get("expected_outcome") and b["outcome"] != b["expected_outcome"]:
                print(f"  outcome ours {b['outcome']!r}, reference {b['expected_outcome']!r}")
        print(f"FAIL: {len(bad)} of {n_pos} positions differ; details in {a.mismatches}")
        return 1
    print(f"OK: {n_pos} positions, {n_moves} reference moves, all legal-move sets identical; "
          f"{n_trans} move transitions and all position outcomes identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
