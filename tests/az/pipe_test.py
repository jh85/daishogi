#!/usr/bin/env python3
"""Pipe-eval end-to-end test. Ported from ../chushogi (tests/az_pipe_test.py,
commit d578c81): the Python eval server (az/eval_server.py, untrained net)
drives C++ self-play games over TCP, once with the legal-moves-only wire
format and once with the full policy (DAISHOGI_EVAL_POLICY=full); the
records are validated like the self-play test, and the two formats must
play the same game.

Usage: pipe_test.py [--az ./daishogi-az] [--gen ./daishogi] [--py python]
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start_server(py, port, checkpoint=None):
    """Start az/eval_server.py and wait for its banner; None if it died."""
    cmd = [py, str(ROOT / "az" / "eval_server.py"), "--port", str(port)]
    if checkpoint:
        cmd += ["--checkpoint", str(checkpoint)]
    server = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    deadline = time.time() + 180
    while time.time() < deadline:
        line = server.stdout.readline()
        if "serving" in line:
            return server
        if server.poll() is not None:
            print("FAIL: eval server died:", line)
            return None
    server.terminate()
    print("FAIL: eval server did not start")
    return None


def selfplay(az, port, out, seed, env=None, sims=16, batch=8):
    """One pipe-eval self-play game -> the parsed record (or None)."""
    worker = subprocess.Popen([az], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              text=True, env=env)
    worker.stdin.write(f"selfplay --games 1 --sims {sims} --batch {batch} --seed {seed} "
                       f"--out {out} --eval pipe --port {port}\nquit\n")
    worker.stdin.flush()
    banner = worker.stdout.readline()
    worker.wait(timeout=600)
    if not banner.startswith("game 0"):
        print("FAIL: worker:", banner)
        return None
    recs = list(Path(out).glob("game_*.json"))
    if len(recs) != 1:
        print("FAIL: no record written")
        return None
    return json.loads(recs[0].read_text())


def validate(rec, gen):
    """Policies sum to one; the moves replay and the ending agrees."""
    if not rec["samples"] or not rec["moves"]:
        return "empty game"
    for smp in rec["samples"][:: max(1, len(rec["samples"]) // 10)]:
        total = sum(p for _, p in smp["policy"])
        if abs(total - 1.0) > 1e-4:
            return f"policy sums to {total}"
    ref = subprocess.Popen([gen, "batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           text=True)
    out, _ = ref.communicate("position sfen " + rec["initial"] + " moves " +
                             " ".join(rec["moves"]) + "\nstatus\nquit\n", timeout=60)
    status = out.strip().split()
    if rec["reason"] == "plycap":
        return None if status == ["status", "ongoing"] else f"engine {status}"
    if status[-1] != rec["reason"]:
        return f"record {rec['reason']} vs engine {status}"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--az", default=str(ROOT / "daishogi-az"))
    ap.add_argument("--gen", default=str(ROOT / "daishogi"))
    ap.add_argument("--py", default=sys.executable)
    a = ap.parse_args()

    port = free_port()
    server = start_server(a.py, port)
    if server is None:
        return 1
    try:
        tmp = Path(tempfile.mkdtemp(prefix="azpipe_"))
        results, games = [], []
        for mode, env in (("legal", None), ("full", dict(os.environ, DAISHOGI_EVAL_POLICY="full"))):
            rec = selfplay(a.az, port, tmp / mode, 5, env)
            if rec is None:
                return 1
            err = validate(rec, a.gen)
            if err:
                print(f"FAIL ({mode}): {err}")
                return 1
            results.append(f"{mode}: {len(rec['moves'])} plies, {rec['reason']}")
            games.append(rec["moves"])
        # Same net, seed and fp16 logits on the legal moves: the same game.
        if games[0] != games[1]:
            print("FAIL: the legal and full wire formats played different games")
            return 1
        print(f"2/2 az pipe-eval games ok ({'; '.join(results)})")
        return 0
    finally:
        server.terminate()
        server.wait(timeout=30)


if __name__ == "__main__":
    sys.exit(main())
