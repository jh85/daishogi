#!/usr/bin/env python3
"""Fresh comparison against the reference implementation (needs refgen).

1. Builds a new corpus with a different seed (reference-played games,
   per-piece, random-scatter and targeted positions) in a temporary
   directory and runs compare.py and replay_games.py on it.
2. Plays random-vs-random games with OUR engine and replays them in the
   reference: every move must be legal there, and the reference must reach
   the same result and final position.
3. Compares perft node counts (depth 1-3, optionally 4) from the start
   position and from positions sampled out of the games.

Usage: live_compare.py --binary ./daishogi --refgen PATH [--seed N]
                       [--games N] [--perft-depth 3]
"""

import argparse
import json
import os
import random
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_corpus  # noqa: E402
from refconv import (sfen_to_wire, state_key, usi_to_ref_move,  # noqa: E402
                     ref_outcome_token, wire_to_sfen)

HERE = os.path.dirname(os.path.abspath(__file__))


def run(cmd):
    print("$ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd).returncode


def our_games(binary, seed, games, max_plies):
    out = subprocess.run([binary, "play", "--seed", str(seed), "--games", str(games),
                          "--max-plies", str(max_plies)],
                         capture_output=True, text=True, check=True).stdout.splitlines()
    res, cur = [], None
    for line in out:
        if line.startswith("game "):
            cur = {"header": line, "cutoff": "cutoff" in line}
            res.append(cur)
        elif line.startswith("sfen "):
            cur["start"] = line[5:]
        elif line.startswith("moves"):
            cur["moves"] = line.split()[1:]
        elif line.startswith("final "):
            cur["final"] = line[6:]
    return res


def outcome_of_header(h):
    if "cutoff" in h:
        return "ongoing"
    if "draw" in h:
        return "draw"
    return "win sente" if "sente wins" in h else "win gote"


def check_our_games(binary, refgen, seed, games, max_plies):
    gs = our_games(binary, seed, games, max_plies)
    inp = "".join(json.dumps({"start": sfen_to_wire(g["start"]),
                              "moves": [usi_to_ref_move(u) for u in g["moves"]]}) + "\n"
                  for g in gs)
    out = subprocess.run([refgen, "replay"], input=inp, capture_output=True, text=True,
                         check=True).stdout.splitlines()
    bad, plies, added = 0, 0, 0
    for g, line in zip(gs, out):
        r = json.loads(line)
        plies += len(g["moves"])
        added += r.get("added", 0)
        if not r["ok"]:
            bad += 1
            print(f"FAIL {g['header']}: reference rejects move {r['plies'] + 1} "
                  f"({g['moves'][r['plies']]}): {r['error']}")
            continue
        last = usi_to_ref_move(g["moves"][-1]) if g["moves"] else None
        ref_final = wire_to_sfen(r["final"], last)
        ours = outcome_of_header(g["header"])
        theirs = ref_outcome_token(r["outcome"])
        if ours != theirs or state_key(ref_final) != state_key(g["final"]):
            bad += 1
            print(f"FAIL {g['header']}: reference result {theirs}, final {ref_final}")
    if bad:
        print(f"FAIL: {bad} of {len(gs)} of our games disagree with the reference")
        return 1
    print(f"OK: {len(gs)} of our random games ({plies} plies) replayed by the reference "
          f"with identical results ({added} moves the reference forbids were played; "
          "see tools/refgen)")
    return 0


def check_perft(binary, refgen, sfens, depth):
    bad = 0
    inp = "".join(json.dumps(sfen_to_wire(s)) + "\n" for s in sfens)
    for d in range(1, depth + 1):
        ref = [json.loads(x)["nodes"] for x in subprocess.run(
            [refgen, "perft", "--depth", str(d)], input=inp, capture_output=True,
            text=True, check=True).stdout.splitlines()]
        eng = subprocess.Popen([binary, "batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               text=True)
        cmds = "".join(f"position sfen {s}\nperft {d}\n" for s in sfens) + "quit\n"
        ours = [int(x.split()[1]) for x in eng.communicate(cmds)[0].splitlines()]
        for s, a, b in zip(sfens, ours, ref):
            if a != b:
                bad += 1
                print(f"FAIL perft {d}: ours {a}, reference {b} for {s}")
        print(f"  perft depth {d}: {len(sfens)} positions, {sum(ref)} nodes")
    if bad:
        return 1
    print("OK: perft counts identical")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./daishogi")
    ap.add_argument("--refgen", required=True)
    ap.add_argument("--seed", type=int, default=1000)
    ap.add_argument("--games", type=int, default=100, help="games per configuration")
    ap.add_argument("--perft-depth", type=int, default=3)
    a = ap.parse_args()
    rc = 0

    print(f"== fresh reference corpus (seed {a.seed}) ==")
    tmp = tempfile.mkdtemp(prefix="dai-live-")
    corpus = make_corpus.build_corpus(a.refgen, a.seed, a.games / 40)
    for name, records in corpus.items():
        make_corpus.write(os.path.join(tmp, f"{name}.jsonl.gz"), records)
    files = sorted(os.path.join(tmp, f) for f in os.listdir(tmp))
    rc |= run([sys.executable, os.path.join(HERE, "compare.py"), "--binary", a.binary,
               "--mismatches", os.path.join(tmp, "mismatches.jsonl")] + files)
    rc |= run([sys.executable, os.path.join(HERE, "replay_games.py"), "--binary", a.binary] + files)

    print(f"== our random games checked by the reference (seed {a.seed}) ==")
    rc |= check_our_games(a.binary, a.refgen, a.seed, a.games, 0)
    rc |= check_our_games(a.binary, a.refgen, a.seed + 77777, max(1, a.games // 4), 300)

    print("== perft ==")
    rng = random.Random(a.seed)
    sample = [r["sfen"] for r in corpus["games"] if "sfen" in r and r.get("expected")]
    sample = [make_corpus.START_SFEN] + rng.sample(sample, min(12, len(sample)))
    rc |= check_perft(a.binary, a.refgen, sample, a.perft_depth)

    if rc == 0:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
        print("ALL OK")
    else:
        print(f"FAILURES (see above); the fresh corpus is kept in {tmp}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
