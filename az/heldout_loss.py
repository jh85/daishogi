#!/usr/bin/env python3
"""Policy / value loss of a checkpoint on games it has not trained on (for
example the iteration being played now), next to the trivial baselines:
value = always predict the data's mean WDL, policy = uniform over legal moves.
The policy loss uses the legal-move softmax, as the search does.

Usage: heldout_loss.py CHECKPOINT GAMES_DIR [--max-positions N]
"""
import argparse
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, str(Path(__file__).resolve().parent))
import buffer as B  # noqa: E402
import model as M  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("checkpoint")
    ap.add_argument("games_dir")
    ap.add_argument("--max-positions", type=int, default=20000)
    a = ap.parse_args()
    state = torch.load(a.checkpoint, map_location="cpu", weights_only=True)
    net = M.DaiNet(**M.net_config(state)).cuda().eval()
    net.load_state_dict(state["model"])
    buf = B.Buffer([Path(a.games_dir)], a.max_positions)
    if len(buf) == 0:
        raise SystemExit(f"no finished games under {a.games_dir}")
    targets = buf.value_targets("z", 0.5)
    lp = lv = n = 0
    lv_draw = n_draw = bce_dec = draw_mass = 0.0
    uniform = 0.0
    for s in range(0, len(buf), 256):
        idx = np.arange(s, min(s + 256, len(buf)))
        packed, prog, (pi, pp) = buf.batch(idx)
        planes = M.unpack_batch_device(packed.tobytes(), prog, "cuda")
        with torch.no_grad(), torch.autocast("cuda", dtype=torch.bfloat16):
            policy, wdl = net(planes)
        logits = policy.float().cpu().numpy()
        for r, (i, p) in enumerate(zip(pi, pp)):
            # The search's prior: softmax over the legal moves only (the
            # recorded policy lists every legal move).
            row = logits[r, i]
            row = row - row.max()
            lp += -(p * (row - np.log(np.exp(row).sum()))).sum()
            uniform += np.log(len(i))
        per = -(torch.from_numpy(targets[idx]) * F.log_softmax(wdl.float().cpu(), 1)).sum(1).numpy()
        lv += per.sum()
        drawn = buf.z[idx] == 0.5
        lv_draw += per[drawn].sum()
        n_draw += drawn.sum()
        wp = F.softmax(wdl.float().cpu(), 1).numpy()
        # decisive positions: the predicted win share among win+loss vs z
        dec = ~drawn
        pw = np.clip(wp[dec, 0] / np.maximum(wp[dec, 0] + wp[dec, 2], 1e-9), 1e-6, 1 - 1e-6)
        zz = buf.z[idx][dec]
        bce_dec += -(zz * np.log(pw) + (1 - zz) * np.log(1 - pw)).sum()
        draw_mass += wp[:, 1].sum()
        n += len(idx)
    mean = targets.mean(0)
    base_v = -(targets * np.log(np.clip(mean, 1e-9, 1))).sum(1).mean()
    print(f"{n} positions from {a.games_dir}")
    print(f"policy CE {lp / n:.3f}   (uniform over legal moves {uniform / n:.3f})")
    print(f"value  CE {lv / n:.3f}   (constant mean-WDL baseline {base_v:.3f}; WDL mean {np.round(mean, 3)})")
    n_dec = n - n_draw
    if n_dec:
        print(f"  decisive positions ({int(n_dec)}): win-vs-loss CE {bce_dec / n_dec:.3f} "
              f"(coin flip 0.693)")
    if n_draw:
        print(f"  drawn-game positions ({int(n_draw)}): value CE {lv_draw / n_draw:.3f}; "
              f"mean predicted draw probability {draw_mass / n:.3f}")


if __name__ == "__main__":
    sys.exit(main())
