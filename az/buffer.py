"""Replay buffer for AZ self-play records.

Ported from ../chushogi (az/buffer.py, commit d578c81).

Reads game JSON records produced by daishogi-az selfplay and serves shuffled
training batches: bit-packed planes (+ progress), sparse visit-count policy
targets, and the three value targets (z / rootQ for soft-Z / A0GB leaf).

Value-target selection lives here: --value-target z|softz|a0gb|blend with
blend y = (1-lambda) * z + lambda * rootQ  (lc0's q-blend recommendation).
"""

import base64
import json
import zlib
from pathlib import Path

import numpy as np

import material as MT
import model as M


class Buffer:
    def __init__(self, games_dirs, max_positions=1_000_000, skip_plies=4,
                 sample_fraction=1.0, cap_score_max=0.0,
                 cap_score_scale=MT.DEFAULT_SCALE):
        """sample_fraction < 1 keeps that share of each game's positions,
        chosen by a hash of (file, ply) so the choice is stable across
        reloads. A game's ~1,000 positions share one result; training on all
        of them lets the value head memorize games (see az/README.md).

        cap_score_max > 0 scores games that hit the ply cap by material
        (az/material.py): their z target becomes a partial win for the side
        ahead, at most cap_score_max. Records without a final position
        (written before it was recorded) stay draws."""
        if max_positions <= 0:
            raise ValueError("max_positions must be positive")
        if not 0 < sample_fraction <= 1:
            raise ValueError("sample_fraction must be in (0, 1]")
        threshold = int(sample_fraction * 2**32)
        self.packed = []      # (M.PACKED_BYTES,) uint8
        self.progress = []    # float16
        self.pol_idx = []     # list of int32 arrays
        self.pol_p = []       # list of float32 arrays
        self.z = []           # float32
        self.rootq = []
        self.a0gb = []
        self.cap_t = []       # material score (stm view) of capped games, else nan
        self.wdl = []         # explicit [win, draw, loss] target (puzzle records), else nan
        self.cap_score_max = cap_score_max
        files = []
        for d in games_dirs:
            files.extend(sorted(Path(d).rglob("game_*.json")))
        # Filenames contain process IDs, not chronological iteration numbers.
        # Read newest first and stop at the window rather than decoding the
        # entire history only to throw most of it away.
        files.sort(key=lambda p: (p.stat().st_mtime_ns, str(p)), reverse=True)
        for f in files:
            try:
                rec = json.loads(f.read_text())
            except (json.JSONDecodeError, OSError):
                continue  # a game being written right now
            t_sente = float("nan")
            if cap_score_max > 0 and rec.get("reason") == "plycap" and rec.get("final"):
                t_sente = MT.cap_score(rec["initial"], rec["final"],
                                       cap_score_max, cap_score_scale)
            for smp in reversed(rec["samples"]):
                if smp["ply"] < skip_plies:
                    continue
                if threshold < 2**32 and zlib.crc32(
                        f"{f.name}:{f.parent}:{smp['ply']}".encode()) >= threshold:
                    continue
                self.packed.append(
                    np.frombuffer(base64.b64decode(smp["planes"]),
                                  dtype=np.uint8).copy())
                self.progress.append(np.float16(smp["progress"]))
                idx = np.array([i for i, _ in smp["policy"]], dtype=np.int64)
                p = np.array([p for _, p in smp["policy"]],
                             dtype=np.float32)
                p = p / max(p.sum(), 1e-9)
                self.pol_idx.append(idx)
                self.pol_p.append(p)
                self.z.append(smp["z"])
                self.rootq.append(smp["rootq"])
                self.a0gb.append(smp["a0gb"])
                self.cap_t.append(t_sente if smp.get("stm", 0) == 0 else -t_sente)
                self.wdl.append(smp.get("wdl", [float("nan")] * 3))
                if len(self.packed) >= max_positions:
                    break
            if len(self.packed) >= max_positions:
                break
        for lst in (self.packed, self.progress, self.pol_idx, self.pol_p,
                    self.z, self.rootq, self.a0gb, self.cap_t, self.wdl):
            lst.reverse()
        self.progress = np.asarray(self.progress, dtype=np.float16)
        self.z = np.asarray(self.z, dtype=np.float32)
        self.rootq = np.asarray(self.rootq, dtype=np.float32)
        self.a0gb = np.asarray(self.a0gb, dtype=np.float32)
        self.cap_t = np.asarray(self.cap_t, dtype=np.float32)
        self.wdl = np.asarray(self.wdl, dtype=np.float32).reshape(-1, 3)

    def __len__(self):
        return len(self.packed)

    def value_targets(self, mode, lam):
        """-> (N, 3) WDL-ish target distribution."""
        if self.cap_score_max > 0 and mode != "z":
            raise ValueError("material scoring of capped games needs --value-target z")
        z, q, a = self.z, self.rootq, self.a0gb
        if mode == "z":
            v = z
        elif mode == "softz":
            v = q
        elif mode == "a0gb":
            v = a
        else:  # blend
            v = (1.0 - lam) * z + lam * q
        # scalar v (win prob, stm view) -> [win, draw, loss] target
        out = np.zeros((len(v), 3), dtype=np.float32)
        out[:, 0] = v
        out[:, 2] = 1.0 - v
        if mode == "z":
            out[z == 0.5] = [0.0, 1.0, 0.0]  # exact draw label
            capped = ~np.isnan(self.cap_t)
            if capped.any():
                t = self.cap_t[capped]
                out[capped] = np.stack([np.maximum(t, 0), 1.0 - np.abs(t),
                                        np.maximum(-t, 0)], axis=1)
            explicit = ~np.isnan(self.wdl[:, 0])
            out[explicit] = self.wdl[explicit]
        elif mode == "blend":  # draws keep their draw mass, scaled by 1-lam
            d = z == 0.5
            out[d] = np.stack([(1.0 - lam) * 0.0 + lam * q[d],
                               np.full(d.sum(), 1.0 - lam),
                               (1.0 - lam) * 0.0 + lam * (1.0 - q[d])],
                              axis=1).astype(np.float32)
        return out

    def batch(self, idx):
        """-> packed planes, progress, sparse policy targets."""
        packed = np.stack([self.packed[i] for i in idx])
        prog = self.progress[idx].astype(np.float32)
        return packed, prog, ([self.pol_idx[i] for i in idx],
                              [self.pol_p[i] for i in idx])
