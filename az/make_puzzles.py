#!/usr/bin/env python3
"""Generates short-mate puzzles for puzzle training and writes them as
training records that az/buffer.py reads like self-play games.

Puzzles are made from real self-play positions. The side not to move (the
defender, with a single royal) loses 2-8 of its pieces within 3 squares of
its royal, and 2-5 of the attacker's pieces are moved to empty squares
within 4 squares of it. tools/puzzlegen then keeps the positions with a
forced mate of at most --max-len plies (royal capture included: a tsume
1-move mate is 3 plies, a 3-move mate 5, a 5-move mate 7) and lists every
first move that keeps the shortest mate.

Each puzzle becomes one sample: policy target uniform over the solution
moves, value target a win for the side to move (z = 1), progress plane from
the source position's ply. Mates of length 1 (a hanging royal) are kept for
at most --hanging-share of the set.

With --contrast CHECKPOINT it writes contrast positions instead: generated
the same way, with at least one checking move but no mate within --max-len
plies. Each is labelled by a search of CHECKPOINT (--label-sims, mate solver
on): value target from the search's root score q (signed t = 2q - 1, WDL
[max(t,0), 1-|t|, max(-t,0)]), policy target its best move. They teach that
a position that merely looks like a puzzle is not automatically won.

Usage: make_puzzles.py --games az/data/run3/games --iters 50-76 --out DIR
       [--count 20000] [--procs 8] [--seed 1] [--max-len 7]
       [--contrast CHECKPOINT --label-sims 256]
"""

import argparse
import base64
import json
import multiprocessing as mp
import random
import socket
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import encode as E  # noqa: E402
import model as M  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
PUZZLEGEN = ROOT / "tools/puzzlegen"
KI, PRINCE = E.KIND_INDEX['KI'], E.KIND_INDEX['+DE']


def sfen_of(pos, ply):
    ranks = []
    for y in range(E.RANKS):
        row, empty = "", 0
        for x in range(E.FILES):
            pc = pos.board[y * E.FILES + x]
            if pc is None:
                empty += 1
                continue
            if empty:
                row, empty = row + str(empty), 0
            code = E.KIND_CODES[pc[0]]
            row += code if pc[1] == 0 else code.lower()
        ranks.append(row + (str(empty) if empty else ""))
    return "/".join(ranks) + f" {pos.side} - {ply + 1}"  # move number = ply + 1


def damage(sfen, rng):
    """One candidate from a real position, or None (defender has two royals)."""
    pos = E.Position(sfen)
    ply = pos.move_number - 1
    dfn = pos.side_to_move ^ 1
    royals = [s for s, pc in enumerate(pos.board) if pc and pc[1] == dfn and pc[0] in (KI, PRINCE)]
    if len(royals) != 1:
        return None
    k = royals[0]
    kx, ky = k % E.FILES, k // E.FILES

    def near(s, r):
        return max(abs(s % E.FILES - kx), abs(s // E.FILES - ky)) <= r

    guards = [s for s, pc in enumerate(pos.board) if pc and pc[1] == dfn and s != k and near(s, 3)]
    for s in rng.sample(guards, min(len(guards), rng.randint(2, 8))):
        pos.board[s] = None
    attackers = [s for s, pc in enumerate(pos.board)
                 if pc and pc[1] != dfn and pc[0] not in (KI, PRINCE)]
    empty = [s for s in range(E.BOARD) if pos.board[s] is None and s != k and near(s, 4)]
    rng.shuffle(empty)
    for s in rng.sample(attackers, min(len(attackers), rng.randint(2, 5))):
        if not empty:
            break
        t = empty.pop()
        pos.board[t], pos.board[s] = pos.board[s], None
    return sfen_of(pos, ply)


def solve_chunk(job):
    sources, seed, reps, max_len, contrast = job
    rng = random.Random(seed)
    cands = [c for s in sources for _ in range(reps) if (c := damage(s, rng))]
    out = subprocess.run([str(PUZZLEGEN), "--max", str(max_len), "--nodes", "100000"]
                         + (["--contrast"] if contrast else []),
                         input="\n".join(cands) + "\n", capture_output=True, text=True,
                         check=True).stdout
    return len(cands), [line.split("\t") for line in out.splitlines() if line]


def sample_sources(games, iters, n, seed):
    """n random positions (ply 4..cap-10) from the given iterations, as SFENs."""
    lo, hi = iters
    files = [f for i in range(lo, hi + 1) for f in (games / f"iter_{i:06d}").rglob("game_*.json")]
    rng = random.Random(seed)
    eng = subprocess.Popen([str(ROOT / "daishogi"), "batch"], stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, text=True)
    per_game = max(1, n // max(len(files), 1) + 1)
    out = []
    for f in rng.sample(files, len(files)):
        rec = json.loads(f.read_text())
        mv = rec["moves"]
        if len(mv) < 12:
            continue
        for k in rng.sample(range(4, min(len(mv), E.PLY_CAP - 10)), min(per_game, len(mv) - 12)):
            eng.stdin.write(f"position sfen {rec['initial']} moves {' '.join(mv[:k])}\nsfen\n")
            eng.stdin.flush()
            out.append(eng.stdout.readline().split(" ", 1)[1].strip())
        if len(out) >= n:
            break
    eng.stdin.write("quit\n")
    eng.stdin.flush()
    return out[:n]


def sample_record(sfen, solutions):
    pos = E.Position(sfen)
    gote = pos.side_to_move == 1
    planes = E.encode_position(pos).reshape(E.NUM_PLANES, E.BOARD)
    packed = np.packbits(planes[:M.BINARY_PLANES].reshape(-1).astype(np.uint8))
    idx = [E.move_to_index(E.flip_usi_move(m) if gote else m) for m in solutions]
    return dict(ply=pos.move_number - 1, stm=pos.side_to_move,
                policy=[[i, 1.0 / len(idx)] for i in idx],
                rootq=1.0, a0gb=1.0, z=1.0,
                progress=float(planes[E.NUM_PLANES - 1, 0]),
                planes=base64.b64encode(packed.tobytes()).decode())


def label_chunk(job):
    """Search each position with the eval server on `port` -> (q, best move)."""
    sfens, port, sims = job
    az = subprocess.Popen([str(ROOT / "daishogi-az")], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, text=True, bufsize=1)

    def cmd(line):
        az.stdin.write(line + "\n")
        az.stdin.flush()
        return az.stdout.readline().strip()

    assert cmd(f"eval-server --port {port}") == "ok"
    out = []
    for s in sfens:
        assert cmd(f"position sfen {s}") == "ok"
        res = json.loads(cmd(f"analyze az --sims {sims} --mate-nodes 5000 --probe-depth 3 "
                             f"--probe-nodes 2000").split(" ", 1)[1])
        out.append((s, res["root_score"], res["best_move"]))
    cmd("quit")
    return out


def start_server(checkpoint):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    proc = subprocess.Popen([sys.executable, str(ROOT / "az/eval_server.py"), "--port", str(port),
                             "--checkpoint", checkpoint, "--max-batch", "64", "--tcp-nodelay"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(120):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return proc, port
        except OSError:
            time.sleep(1)
    proc.kill()
    raise RuntimeError("eval server did not start")


def contrast_record(sfen, q, best):
    smp = sample_record(sfen, [best])
    t = 2.0 * q - 1.0
    smp.update(z=0.5, rootq=q, a0gb=q, wdl=[max(t, 0.0), 1.0 - abs(t), max(-t, 0.0)])
    return smp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--games", default=str(ROOT / "az/data/run3/games"))
    ap.add_argument("--iters", required=True, help="source iterations, e.g. 50-76")
    ap.add_argument("--out", required=True)
    ap.add_argument("--count", type=int, default=20000)
    ap.add_argument("--hanging-share", type=float, default=0.2)
    ap.add_argument("--max-len", type=int, default=7)
    ap.add_argument("--reps", type=int, default=20, help="candidates per source position")
    ap.add_argument("--procs", type=int, default=8)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--contrast", default="", metavar="CHECKPOINT",
                    help="write search-labelled contrast positions instead of puzzles")
    ap.add_argument("--label-sims", type=int, default=256)
    a = ap.parse_args()
    if not PUZZLEGEN.exists():
        ap.error(f"{PUZZLEGEN} missing: run make tools/puzzlegen")
    iters = tuple(int(x) for x in a.iters.split("-"))
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)

    want_mates = a.count if a.contrast else int(a.count * (1 - a.hanging_share))
    want_hang = 0 if a.contrast else a.count - want_mates
    mates, hanging, seen, cands = [], [], set(), 0
    rnd = 0
    with mp.Pool(a.procs) as pool:
        while len(mates) < want_mates:
            sources = sample_sources(Path(a.games), iters, 4000, a.seed * 1000 + rnd)
            chunks = [(sources[i::a.procs], a.seed * 100003 + rnd * 97 + i, a.reps, a.max_len,
                       bool(a.contrast)) for i in range(a.procs)]
            for n, rows in pool.imap_unordered(solve_chunk, chunks):
                cands += n
                for sfen, length, _nodes, _legal, sol in rows:
                    board = sfen.rsplit(" ", 1)[0]
                    if board in seen:
                        continue
                    seen.add(board)
                    (hanging if length == "1" else mates).append((sfen, int(length), sol.split()))
            rnd += 1
            print(f"[puzzles] round {rnd}: {cands} candidates, {len(mates)} mates, "
                  f"{len(hanging)} hanging royals", flush=True)
    rng = random.Random(a.seed)
    if a.contrast:
        rng.shuffle(mates)
        mates = mates[:a.count]
        server, port = start_server(a.contrast)
        try:
            with mp.Pool(a.procs) as pool:
                jobs = [([m[0] for m in mates[i::a.procs]], port, a.label_sims) for i in range(a.procs)]
                labelled = [x for part in pool.map(label_chunk, jobs) for x in part]
        finally:
            server.kill()
        for i in range(0, len(labelled), 1000):
            part = labelled[i:i + 1000]
            rec = dict(reason="contrast", result=0, initial="", moves=[],
                       samples=[contrast_record(s, q, b) for s, q, b in part])
            (out / f"game_contrast_{i // 1000:04d}.json").write_text(json.dumps(rec))
        checks = {m[0]: m[2] for m in mates}
        with open(out / "contrast.tsv", "w") as f:
            for s, q, b in labelled:
                f.write(f"{s}\t0\t0\t0\t{' '.join(checks[s])}\t{q:.4f}\n")
        qs = sorted(q for _, q, _ in labelled)
        print(f"[puzzles] wrote {len(labelled)} contrast positions to {out} "
              f"(search root score median {qs[len(qs) // 2]:.3f}, "
              f"10%/90% {qs[len(qs) // 10]:.3f}/{qs[9 * len(qs) // 10]:.3f})")
        return
    rng.shuffle(hanging)
    chosen = mates[:want_mates] + hanging[:want_hang]
    rng.shuffle(chosen)
    lengths = {}
    for i in range(0, len(chosen), 1000):
        part = chosen[i:i + 1000]
        rec = dict(reason="puzzle", result=0, initial="", moves=[],
                   samples=[sample_record(s, sol) for s, _, sol in part])
        (out / f"game_puzzles_{i // 1000:04d}.json").write_text(json.dumps(rec))
        for _, length, _ in part:
            lengths[length] = lengths.get(length, 0) + 1
    with open(out / "puzzles.tsv", "w") as f:
        for s, length, sol in chosen:
            f.write(f"{s}\t{length}\t0\t0\t{' '.join(sol)}\n")
    print(f"[puzzles] wrote {len(chosen)} puzzles to {out} "
          f"(by length: {dict(sorted(lengths.items()))}; {cands} candidates)")


if __name__ == "__main__":
    main()
