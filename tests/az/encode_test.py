#!/usr/bin/env python3
"""C++ / Python encoding agreement. Ported from ../chushogi
(tests/az_encode_test.py, commit d578c81).

  1. Planes: the C++ encoder (daishogi-az `encode`) and the Python mirror
     (az/encode.py) produce identical planes.
  2. Move mapping: every legal move (from `daishogi batch` `legal`, flipped
     to the canonical frame when Gote is to move) maps to the same policy
     index in C++ (`legal-index`) and Python (move_to_index); Python's
     index_to_move and C++'s `index-move` both invert it.
  3. Repetition planes: positions along the committed rule games (fourfold
     repetition, perpetual check), whose repeat counts are computed here
     from the replayed SFENs.

Positions: the initial position and a stride sample of the committed corpus
(random-game positions, scattered positions with promoted pieces, every
piece kind on its own, targeted lion and promotion cases).

Usage: encode_test.py [--az ./daishogi-az] [--gen ./daishogi] [--stride 10]
"""

import argparse
import gzip
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "az"))
import numpy as np  # noqa: E402

import encode as enc  # noqa: E402

INITIAL = ("laknstircosigokigosicoirstknla/rc1cs1fl1btdebt1fl1cs1rc/"
           "1vo1ab1ewphlikrew1ab1vo1/rofdsmvmbidhdkfkdkdhbivmsmfdro/"
           "papapapapapapapapapapapapapapa/4gb5gb4/15/15/15/4GB5GB4/"
           "PAPAPAPAPAPAPAPAPAPAPAPAPAPAPA/ROFDSMVMBIDHDKFKDKDHBIVMSMFDRO/"
           "1VO1AB1EWKRLIPHEW1AB1VO1/RC1CS1FL1BTDEBT1FL1CS1RC/"
           "LAKNSTIRCOSIGOKIGOSICOIRSTKNLA b - 1")


class Cli:
    def __init__(self, command):
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True, bufsize=1)

    def cmd(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        return self.proc.stdout.readline().strip()

    def send(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def close(self):
        self.send("quit")
        self.proc.wait(timeout=10)


def corpus_positions(stride):
    yield "initial", INITIAL
    data = ROOT / "tests" / "data"
    for name, step in (("games", 2 * stride), ("scatter", stride),
                       ("pieces", stride), ("targeted", 1)):
        with gzip.open(data / f"{name}.jsonl.gz", "rt") as f:
            records = (json.loads(line) for line in f)
            for i, rec in enumerate(r for r in records if "sfen" in r):
                if i % step == 0:
                    yield rec["id"], rec["sfen"]


def state(sfen):
    """Repetition identity: board, side to move, lion-capture square."""
    return " ".join(sfen.split()[:3])


class Checker:
    def __init__(self, az, gen):
        self.az, self.gen = az, gen
        self.total = self.failures = 0

    def fail(self, msg):
        self.failures += 1
        print("FAIL", msg)

    def planes(self, label, sfen, reps, ply):
        cpp = np.array(self.az.cmd("encode").split()[1:], dtype=np.float32)
        py = enc.encode_position(enc.Position(sfen), reps, ply).reshape(-1)
        self.total += 1
        if cpp.shape != py.shape or not np.allclose(cpp, py, atol=1e-6):
            diff = np.nonzero(~np.isclose(cpp, py, atol=1e-6))[0] if cpp.shape == py.shape else []
            self.fail(f"planes {label}: {len(diff)} diffs, first planes "
                      f"{sorted(set(int(d) // enc.BOARD for d in diff[:20]))}")
            return False
        return True

    def moves(self, label, sfen):
        pos = enc.Position(sfen)
        legal = self.gen.cmd("legal").split()[1:]
        flip = pos.side_to_move == 1
        canon = [enc.flip_usi_move(m) if flip else m for m in legal]
        want = sorted(enc.move_to_index(m) for m in canon)
        got = sorted(int(i) for i in self.az.cmd("legal-index").split()[1:])
        self.total += 1
        if want != got:
            missing = sorted(set(want) - set(got))
            extra = sorted(set(got) - set(want))
            self.fail(f"indices {label}: missing={missing[:5]} extra={extra[:5]}")
            return
        if len(set(want)) != len(want):
            self.fail(f"indices {label}: two legal moves share an index")
        self.total += 1
        for m in canon:
            if enc.index_to_move(enc.move_to_index(m)) != m:
                self.fail(f"python roundtrip {label}: {m}")
                return
        self.total += 1
        if canon:
            back = self.az.cmd("index-move " + " ".join(
                str(enc.move_to_index(m)) for m in canon)).split()[1:]
            if back != canon:
                bad = [(a, b) for a, b in zip(canon, back) if a != b]
                self.fail(f"C++ index-move {label}: {bad[:3]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--az", default=str(ROOT / "daishogi-az"))
    ap.add_argument("--gen", default=str(ROOT / "daishogi"))
    ap.add_argument("--stride", type=int, default=10)
    a = ap.parse_args()

    az, gen = Cli([a.az]), Cli([a.gen, "batch"])
    c = Checker(az, gen)
    positions = 0
    lion_capture = gote = 0
    for label, sfen in corpus_positions(a.stride):
        assert az.cmd(f"position sfen {sfen}") == "ok", sfen
        gen.send(f"position sfen {sfen}")
        positions += 1
        pos = enc.Position(sfen)
        lion_capture += pos.lion_capture is not None
        gote += pos.side_to_move == 1
        if c.planes(label, sfen, 0, 0):
            c.moves(label, sfen)

    # Repetition planes along the rule games.
    rep_positions = 0
    with gzip.open(ROOT / "tests" / "data" / "rule_games.jsonl.gz", "rt") as f:
        for line in f:
            game = json.loads(line)
            if game.get("type") != "game":
                continue
            seen = {}
            # Stop before a move the rules reject (illegal_at).
            legal_plies = game.get("illegal_at")
            if legal_plies is None:
                legal_plies = len(game["moves"])
            for n in range(legal_plies + 1):
                tail = " moves " + " ".join(game["moves"][:n]) if n else ""
                assert az.cmd(f"position sfen {game['start']}{tail}") == "ok"
                sfen = az.cmd("sfen")[5:]
                key = state(sfen)
                reps = seen.get(key, 0)
                seen[key] = reps + 1
                rep_positions += reps > 0
                c.planes(f"{game['id']} ply {n}", sfen, reps, n)

    az.close()
    gen.close()
    print(f"{c.total - c.failures}/{c.total} az encode checks passed "
          f"({positions} positions, {gote} with Gote to move, {lion_capture} with "
          f"the lion-capture state, {rep_positions} repeated positions in rule games)")
    return 1 if c.failures else 0


if __name__ == "__main__":
    sys.exit(main())
