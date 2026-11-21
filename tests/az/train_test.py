#!/usr/bin/env python3
"""Trainer smoke test. Ported from ../chushogi (tests/az_train_test.py,
commit d578c81): a few uniform-eval self-play games feed the trainer; a
resumed run must continue the optimizer state and step counter; the
checkpoint must load into the eval server and drive a pipe-eval game.

Usage: train_test.py [--az ./daishogi-az] [--gen ./daishogi] [--py python]
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pipe_test import ROOT, free_port, selfplay, start_server, validate  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--az", default=str(ROOT / "daishogi-az"))
    ap.add_argument("--gen", default=str(ROOT / "daishogi"))
    ap.add_argument("--py", default=sys.executable)
    a = ap.parse_args()

    tmp = Path(tempfile.mkdtemp(prefix="aztrain_"))
    games = tmp / "games"
    ckpt = tmp / "current.pt"
    train = [a.py, str(ROOT / "az" / "train.py"), "--games-dirs", str(games), "--out", str(ckpt),
             "--batch", "32", "--d-model", "64", "--blocks", "2", "--heads", "4"]

    # 1. uniform-eval games
    w = subprocess.Popen([a.az], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    w.stdin.write(f"selfplay --games 3 --sims 8 --batch 4 --seed 11 --out {games}\nquit\n")
    w.stdin.flush()
    for _ in range(3):
        line = w.stdout.readline()
        assert line.startswith("game"), line
    w.wait(timeout=300)

    # 2. train briefly (a small net: the network size travels in the checkpoint)
    t = subprocess.run(train + ["--steps", "30", "--save-every", "15", "--log-every", "10",
                                "--warmup-steps", "5"],
                       capture_output=True, text=True, timeout=900)
    if t.returncode != 0 or not ckpt.exists():
        print(t.stdout[-2000:], t.stderr[-2000:])
        print("FAIL: trainer exited nonzero or wrote no checkpoint")
        return 1
    losses = [float(x.split("loss ")[1].split(" ")[0])
              for x in t.stdout.splitlines() if " loss " in x and "val" not in x]
    print(f"[test] trainer losses: {losses}")

    # Resume must continue AdamW's moments and update counter, not restart them.
    t = subprocess.run(train + ["--init-from", str(ckpt), "--steps", "2"],
                       capture_output=True, text=True, timeout=900)
    if t.returncode != 0 or "restored_adamw" not in t.stdout:
        print(t.stdout[-2000:], t.stderr[-2000:])
        print("FAIL: resume did not restore the optimizer")
        return 1
    check = subprocess.run(
        [a.py, "-c", "import sys, torch; "
         "s=torch.load(sys.argv[1],weights_only=True,map_location='cpu'); "
         "assert s['step']==32, s['step']; "
         "assert all(int(v['step'])==32 for v in s['optimizer']['state'].values()); "
         "assert s['config']['d_model']==64 and s['config']['blocks']==2",
         str(ckpt)], capture_output=True, text=True)
    if check.returncode != 0:
        print(check.stderr)
        print("FAIL: checkpoint step / optimizer state / config")
        return 1

    # 3. the trained checkpoint drives a pipe game
    port = free_port()
    server = start_server(a.py, port, ckpt)
    if server is None:
        return 1
    try:
        rec = selfplay(a.az, port, tmp / "g2", 13, sims=8, batch=4)
        err = "no game" if rec is None else validate(rec, a.gen)
        if err:
            print(f"FAIL: trained-net game: {err}")
            return 1
        print(f"1/1 az train test ok (trained-net game: {len(rec['moves'])} plies, "
              f"{rec['reason']})")
        return 0
    finally:
        server.terminate()
        server.wait(timeout=30)


if __name__ == "__main__":
    sys.exit(main())
