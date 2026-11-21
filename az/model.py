"""AlphaZero network for dai shogi (PyTorch).

Ported from ../chushogi (az/model.py, commit d578c81), resized for the
15x15 board. JHBR3-BT4-style transformer over the 225 board squares:
  - per-square input embedding from the 114 input planes
  - pre-norm transformer trunk
  - policy head: Q@K^T per-square pair projections -> the 225x225 from-to
    matrix (x2: normal + promotion), plus a per-square MLP producing the
    64 double-move (two-step lion-power) logits per from-square
  - value head: pooled -> 3-way WDL

Policy layout (115,650): [from-to 50,625 | from-to-promote 50,625 |
double-move 14,400], matching src/az/encode.cpp.
"""

import torch
import torch.nn as nn

FILES = RANKS = 15
BOARD = FILES * RANKS                       # 225
NUM_PLANES = 114
BINARY_PLANES = NUM_PLANES - 1              # the progress plane is sent separately
PACKED_BYTES = (BINARY_PLANES * BOARD + 7) // 8   # 3,179
POLICY_MOVE = BOARD * BOARD
POLICY_DOUBLE = BOARD * 64
POLICY_SIZE = 2 * POLICY_MOVE + POLICY_DOUBLE     # 115,650


class Block(nn.Module):
    def __init__(self, d, heads, ffn_mult=4):
        super().__init__()
        self.n1 = nn.RMSNorm(d)
        self.attn = nn.MultiheadAttention(d, heads, batch_first=True)
        self.n2 = nn.RMSNorm(d)
        self.ffn = nn.Sequential(nn.Linear(d, ffn_mult * d), nn.GELU(),
                                 nn.Linear(ffn_mult * d, d))

    def forward(self, x):
        h = self.n1(x)
        x = x + self.attn(h, h, h, need_weights=False)[0]
        x = x + self.ffn(self.n2(x))
        return x


class DaiNet(nn.Module):
    def __init__(self, d_model=256, blocks=6, heads=8):
        super().__init__()
        self.embed = nn.Linear(NUM_PLANES, d_model)
        self.pos = nn.Parameter(torch.randn(1, BOARD, d_model) * 0.02)
        self.trunk = nn.ModuleList(Block(d_model, heads)
                                   for _ in range(blocks))
        self.norm = nn.RMSNorm(d_model)
        self.q1 = nn.Linear(d_model, 64)
        self.k1 = nn.Linear(d_model, 64)
        self.q2 = nn.Linear(d_model, 64)
        self.k2 = nn.Linear(d_model, 64)
        self.dbl = nn.Sequential(nn.Linear(d_model, 128), nn.GELU(),
                                 nn.Linear(128, 64))
        self.vpool = nn.Linear(d_model, 8)
        self.vfc = nn.Sequential(nn.Linear(8 * BOARD, 256), nn.GELU(),
                                 nn.Linear(256, 3))

    def forward(self, planes):
        """planes: (B, 114, 15, 15) -> (policy logits (B, 115650), wdl (B, 3))."""
        b = planes.shape[0]
        x = planes.reshape(b, NUM_PLANES, BOARD).permute(0, 2, 1)  # B,225,114
        x = self.embed(x) + self.pos
        for blk in self.trunk:
            x = blk(x)
        x = self.norm(x)
        m1 = torch.matmul(self.q1(x), self.k1(x).transpose(1, 2))  # B,225,225
        m2 = torch.matmul(self.q2(x), self.k2(x).transpose(1, 2))
        dbl = self.dbl(x)                                          # B,225,64
        policy = torch.cat([m1.reshape(b, -1), m2.reshape(b, -1),
                            dbl.reshape(b, -1)], dim=1)
        v = self.vpool(x).reshape(b, 8 * BOARD)
        wdl = self.vfc(v)
        return policy, wdl


def net_config(state):
    """Network size saved by train.py in a checkpoint (defaults otherwise)."""
    cfg = state.get("config", {}) if isinstance(state, dict) else {}
    return dict(d_model=cfg.get("d_model", 256), blocks=cfg.get("blocks", 6),
                heads=cfg.get("heads", 8))


def unpack_batch(raw, progress):
    """Bit-packed planes -> torch tensor (B, 114, 15, 15).

    raw: bytes, B * PACKED_BYTES; progress: float array of B (ply / cap).
    """
    import numpy as np
    rows = np.frombuffer(raw, dtype=np.uint8).reshape(-1, PACKED_BYTES)
    b = rows.shape[0]
    bits = np.unpackbits(rows, axis=1)[:, :BINARY_PLANES * BOARD]
    planes = np.zeros((b, NUM_PLANES, RANKS, FILES), dtype=np.float32)
    planes[:, :BINARY_PLANES] = bits.reshape(b, BINARY_PLANES, RANKS, FILES)
    planes[:, BINARY_PLANES] = np.asarray(progress, dtype=np.float32)[:, None, None]
    return torch.from_numpy(planes)


def unpack_batch_device(raw, progress, device, dtype=torch.float32):
    """unpack_batch on the device: only the packed bytes are uploaded and the
    bits are expanded there (MSB first, as numpy.unpackbits)."""
    import numpy as np
    rows = torch.from_numpy(np.frombuffer(raw, dtype=np.uint8).reshape(-1, PACKED_BYTES).copy())
    rows = rows.to(device, non_blocking=True)
    b = rows.shape[0]
    shifts = torch.arange(7, -1, -1, device=device, dtype=torch.uint8)
    bits = ((rows.unsqueeze(-1) >> shifts) & 1).reshape(b, -1)[:, :BINARY_PLANES * BOARD]
    planes = torch.empty((b, NUM_PLANES, RANKS, FILES), device=device, dtype=dtype)
    planes[:, :BINARY_PLANES] = bits.reshape(b, BINARY_PLANES, RANKS, FILES).to(dtype)
    prog = torch.as_tensor(np.asarray(progress, dtype=np.float32)).to(device)
    planes[:, BINARY_PLANES] = prog.to(dtype)[:, None, None]
    return planes
