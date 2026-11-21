#!/usr/bin/env python3
"""Mutation check for the test suite itself.

Builds copies of the engine with one rule deliberately broken and verifies
that the corpus comparison (compare.py + replay_games.py) detects every one.
A mutant that survives means the corpus does not cover that rule.

Usage: mutation_check.py [--work DIR]   (run from the project root)
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

MUTANTS = {
    "lion-capture: no 付け喰い pawn/go-between exception":
        ("movegen.cpp", "if (first && !isPawnOrGoBetween(first)) return true;", "if (first) return true;"),
    "lion-capture: a pawn / go-between taken first still blocks (reference behavior)":
        ("movegen.cpp", "  after.board[mid] = 0;\n", ""),
    "lion-capture: a pawn / go-between taken first no longer protects":
        ("movegen.cpp", "if (s != from && isPawnOrGoBetween(p)", "if (false && isPawnOrGoBetween(p)"),
    "lion-capture: adjacent lion not exempt on the second step (reference behavior)":
        ("movegen.cpp", "  if (dist(from, to) == 1) return true;\n", ""),
    "lion-capture: 先獅子 ignored":
        ("movegen.cpp", "return pos.lionCapture == kNoSquare;", "return true;"),
    "lion-capture: adjacent lions not exempt":
        ("movegen.cpp", "return dist(from, to) == 1 || !lionProtected(pos, to, from);",
         "return !lionProtected(pos, to, from);"),
    "lion-capture: no x-ray through the capturing lion":
        ("movegen.cpp", "return attackedBy(pos, colorOf(pos.board[target]), target, vacant);",
         "return attackedBy(pos, colorOf(pos.board[target]), target, kNoSquare);"),
    "falcon/eagle: second hop uses the pre-move 先獅子 state":
        ("movegen.cpp", "return !isLionPiece(first);",
         "return !isLionPiece(first) && pos.lionCapture == kNoSquare;"),
    "two-step move continues after capturing the last royal":
        ("movegen.cpp", "if (victim && info(kindOf(victim)).royal && theirRoyals == 1) continue;", ""),
    "promotion: not allowed when leaving the zone":
        ("movegen.cpp", "(relRank(s, us) < kPromotionRanks ||", "(false ||"),
    "promotion: zone of 4 ranks":
        ("types.h", "constexpr int kPromotionRanks = 5;", "constexpr int kPromotionRanks = 4;"),
    "no file limit for pawns / go-betweens":
        ("movegen.cpp", "if (!limit) return true;", "return true;"),
    "knight dead zone of 1 rank":
        ("types.cpp", "t[Knight].deadRanks = 2;", "t[Knight].deadRanks = 1;"),
    "violent ox unlimited range":
        ("types.cpp", "rays({{0, -1}, {1, 0}, {-1, 0}, {0, 1}}, 2), GoldVO",
         "rays({{0, -1}, {1, 0}, {-1, 0}, {0, 1}}), GoldVO"),
    "blind tiger moves straight forward":
        ("types.cpp", "{{1, -1}, {-1, -1}, {1, 0}, {-1, 0}, {1, 1}, {0, 1}, {-1, 1}}, {},\n      FlyingStag",
         "{{1, -1}, {0, -1}, {-1, -1}, {1, 0}, {-1, 0}, {1, 1}, {0, 1}, {-1, 1}}, {},\n      FlyingStag"),
    "先獅子 state kept from the first hop":
        ("position.cpp", "    hop(m.mid, m.to);\n",
         "    { int16_t lc = lionCapture; hop(m.mid, m.to);"
         " if (lc != kNoSquare && lionCapture == kNoSquare) lionCapture = lc; }\n"),
    "lion capture by a lion sets 先獅子":
        ("position.cpp", "!info(kindOf(mover)).lion)", "true)"),
    "repetition: no perpetual-check rule":
        ("game.cpp", "    if (all)\n", "    if (false)\n"),
    "no-legal-move position is not a loss":
        ("game.cpp", "  if (generateLegal(current()).empty())\n", "  if (false)\n"),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=None)
    a = ap.parse_args()
    work = a.work or tempfile.mkdtemp(prefix="dai-mutants-")
    data = sorted(os.path.join("tests/data", f) for f in os.listdir("tests/data") if f.endswith(".gz"))
    survivors = 0
    for i, (name, (f, old, new)) in enumerate(MUTANTS.items()):
        d = os.path.join(work, f"m{i}")
        shutil.rmtree(d, ignore_errors=True)
        shutil.copytree("src", d)
        path = os.path.join(d, f)
        text = open(path).read()
        assert old in text, f"mutant '{name}' does not apply"
        open(path, "w").write(text.replace(old, new, 1))
        srcs = [os.path.join(d, x + ".cpp") for x in "types position notation movegen game main".split()]
        subprocess.run(["g++", "-std=c++17", "-O2", "-o", d + "/bin"] + srcs, check=True)
        caught = []
        for script in ("compare.py", "replay_games.py"):
            r = subprocess.run([sys.executable, "tests/" + script, "--binary", d + "/bin"] +
                               (["--mismatches", d + "/mm.jsonl", "--max-report", "0"]
                                if script == "compare.py" else []) + data,
                               capture_output=True, text=True)
            if r.returncode != 0:
                last = [x for x in r.stdout.splitlines() if x.startswith("FAIL:")]
                caught.append(f"{script}: {last[-1][6:] if last else 'failed'}")
        survivors += not caught
        print(f"{'caught  ' if caught else 'SURVIVED'} {name}")
        for c in caught:
            print(f"           {c}")
    print(f"{len(MUTANTS) - survivors}/{len(MUTANTS)} mutants detected")
    return 1 if survivors else 0


if __name__ == "__main__":
    sys.exit(main())
