"""The short-horizon material target, the material head, loading checkpoints
from before it, and the optimizer migration for its new parameters."""
import base64
import copy
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'az'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import material as MT  # noqa: E402
import model as M  # noqa: E402
from buffer import Buffer  # noqa: E402
from optimizers import Optimizers  # noqa: E402
from material_test import INITIAL  # noqa: E402

torch.set_num_threads(1)
ARMY = MT.material(INITIAL)[0]


class ChangeTargetTest(unittest.TestCase):
    def test_sign_horizon_and_game_end(self):
        bal = [0, 0, 1000, 1000, 960, 960]   # Sente wins a lion at ply 2, loses a pawn at 4
        t = MT.change_target(bal, 0, 0, ARMY, horizon=3, scale=0.05)
        self.assertAlmostEqual(t, math.tanh(1000 / ARMY / 0.05), places=6)
        self.assertAlmostEqual(MT.change_target(bal, 0, 1, ARMY, 3, 0.05), -t, places=6)  # Gote's view
        self.assertAlmostEqual(MT.change_target(bal, 2, 0, ARMY, 3, 0.05),
                               math.tanh(-40 / ARMY / 0.05), places=6)
        self.assertEqual(MT.change_target(bal, 3, 0, ARMY, 100, 0.05),        # to the game's end
                         math.tanh(-40 / ARMY / 0.05))
        self.assertEqual(MT.change_target(bal, 0, 0, ARMY, 1, 0.05), 0.0)     # nothing within 1 ply

    def test_buffer_targets(self):
        planes = base64.b64encode(bytes(M.PACKED_BYTES)).decode()
        bal = [0] * 10 + [1000] * 11              # 20 plies; Sente wins a lion at ply 10
        samples = [dict(ply=p, stm=p % 2, planes=planes, progress=0, policy=[[0, 1.0]],
                        z=.5, rootq=.5, a0gb=.5) for p in (4, 5, 12)]
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            rec = dict(initial=INITIAL, reason="plycap", result=0, samples=samples)
            (d / "game_1_0000.json").write_text(json.dumps(rec))             # no trajectory
            (d / "game_2_0000.json").write_text(json.dumps(dict(rec, material=bal)))
            mat = Buffer([d], material_horizon=8).mat_t
            off = Buffer([d]).mat_t
        got = sorted(round(float(t), 6) for t in mat if t == t)
        win = round(math.tanh(1000 / ARMY / 0.05), 6)
        # ply 4: 4..12 sees the lion (+, Sente); ply 5: Gote's view (-); ply 12: no change.
        self.assertEqual(got, [-win, 0.0, win])
        self.assertEqual(sum(1 for t in mat if t != t), 3)   # the record without a trajectory
        self.assertTrue(all(t != t for t in off))   # horizon 0: no targets


class HeadAndLoadingTest(unittest.TestCase):
    def setUp(self):
        torch.manual_seed(3)
        self.net = M.DaiNet(16, 1, 4)
        self.old = {k: v for k, v in self.net.state_dict().items()
                    if not k.startswith(M.AUX_PREFIXES)}

    def test_forward(self):
        x = torch.randn(2, 114, 15, 15)
        p, v = self.net(x)
        p2, v2, m = self.net(x, material=True)
        torch.testing.assert_close(p, p2)
        self.assertEqual(tuple(m.shape), (2,))
        self.assertTrue(bool((m.abs() < 1).all()))

    def test_old_checkpoint_loads(self):
        fresh = M.DaiNet(16, 1, 4)
        self.assertFalse(M.load_weights(fresh, self.old))
        self.assertTrue(M.load_weights(fresh, self.net.state_dict()))
        bad = dict(self.old)
        bad.pop("vpool.weight")
        with self.assertRaises(RuntimeError):
            M.load_weights(fresh, bad)

    def test_optimizer_migration_keeps_old_moments(self):
        names = [n for n, _ in self.net.named_parameters() if not n.startswith(M.AUX_PREFIXES)]
        params = [p for n, p in self.net.named_parameters() if not n.startswith(M.AUX_PREFIXES)]
        old = torch.optim.AdamW(params, lr=.001, weight_decay=.01)
        p, v = self.net(torch.randn(2, 114, 15, 15))
        (p.square().mean() + v.square().mean()).backward()
        old.step()
        opt = Optimizers(self.net)
        opt.load_checkpoint(dict(optimizer=copy.deepcopy(old.state_dict()),
                                 optimizer_param_names=names))
        self.assertEqual(opt.migration, 'restored_adamw_new_parameters_fresh')
        for n, q in self.net.named_parameters():
            if n.startswith(M.AUX_PREFIXES):
                self.assertNotIn(q, opt.adamw.state)
            else:
                torch.testing.assert_close(opt.adamw.state[q]['exp_avg'], old.state[q]['exp_avg'],
                                           rtol=0, atol=0)
        with self.assertRaises(ValueError):   # a removed parameter is not a migration
            opt2 = Optimizers(M.DaiNet(16, 1, 4))
            opt2.load_checkpoint(dict(optimizer=copy.deepcopy(old.state_dict()),
                                      optimizer_param_names=names[:-1] + ["gone"]))


if __name__ == '__main__':
    unittest.main()
