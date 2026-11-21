#!/usr/bin/env python3
"""Self-play throughput: one batching eval server + W daishogi-az workers.

Starts az/eval_server.py with a randomly initialized network of the given
size (or --checkpoint), runs W self-play workers against it for --secs
seconds after a warm-up, and reports network evaluations per second, the
mean GPU batch and the implied plies per second (one evaluation per
simulation). Games are not finished; the records are discarded.

Usage: bench_selfplay.py [--workers 16] [--sims 64] [--batch 16]
       [--max-batch 256] [--net 256,6,8 | --checkpoint PT] [--secs 30]
"""

import argparse
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import torch

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "az"))
import model as M  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--az", default=str(ROOT / "daishogi-az"))
    ap.add_argument("--workers", type=int, default=16)
    ap.add_argument("--sims", type=int, default=64)
    ap.add_argument("--batch", type=int, default=16, help="MCTS leaves per worker request")
    ap.add_argument("--max-batch", type=int, default=256, help="server request batching")
    ap.add_argument("--net", default="256,6,8", help="d_model,blocks,heads of a random net")
    ap.add_argument("--checkpoint", default="")
    ap.add_argument("--secs", type=float, default=30)
    ap.add_argument("--profile", action="store_true", help="print the server's eval profile")
    ap.add_argument("--extra", default="", help="more selfplay options, e.g. "
                    "'--mate-nodes 5000 --probe-depth 3 --full-prob 0.25'")
    a = ap.parse_args()

    tmp = Path(tempfile.mkdtemp(prefix="azbench_"))
    ckpt = a.checkpoint
    if not ckpt:
        d, b, h = map(int, a.net.split(","))
        torch.manual_seed(0)
        ckpt = tmp / "net.pt"
        torch.save(dict(model=M.DaiNet(d, b, h).state_dict(), step=0,
                        config=dict(d_model=d, blocks=b, heads=h)), ckpt)
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    srv = subprocess.Popen(
        [sys.executable, str(ROOT / "az/eval_server.py"), "--port", str(port),
         "--checkpoint", str(ckpt), "--max-batch", str(a.max_batch), "--stats-interval", "5",
         "--tcp-nodelay"] + (["--profile-eval"] if a.profile else []),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    workers = []
    try:
        while "serving" not in (line := srv.stdout.readline()):
            if srv.poll() is not None:
                raise SystemExit(f"eval server died: {line}")
        for i in range(a.workers):
            w = subprocess.Popen([a.az], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                 text=True)
            w.stdin.write(f"selfplay --games 100000 --sims {a.sims} --batch {a.batch} "
                          f"--seed {i * 7919} --out {tmp}/w{i} --eval pipe --port {port} {a.extra}\n")
            w.stdin.flush()
            workers.append(w)
        profile = [""]

        def stats():  # the batcher's cumulative counters, every 5 s
            while True:
                line = srv.stdout.readline()
                if not line:
                    raise SystemExit("eval server exited")
                if "eval-profile" in line:
                    profile[0] = line.strip()
                m = re.search(r"'positions': (\d+), 'batches': (\d+)", line)
                if m:
                    return time.time(), int(m.group(1)), int(m.group(2))

        stats()  # warm-up
        t0, p0, b0 = stats()
        while True:
            t1, p1, b1 = stats()
            if t1 - t0 >= a.secs:
                break
        rate = (p1 - p0) / (t1 - t0)
        print(f"net {a.checkpoint or a.net}  workers {a.workers}  sims {a.sims}  batch {a.batch}  "
              f"max-batch {a.max_batch}: {rate:.0f} evals/s, mean GPU batch "
              f"{(p1 - p0) / max(b1 - b0, 1):.1f}, ~{rate / a.sims:.1f} plies/s", flush=True)
        if a.profile:
            print(profile[0])
    finally:
        for w in workers:
            w.kill()
        srv.terminate()
        srv.wait(timeout=30)
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
