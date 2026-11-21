"""Replay window: file modification time, not PID filenames, decides data
recency. Ported from ../chushogi (tests/az_buffer_test.py, commit d578c81);
also checks that policy indices above 65,535 survive loading."""
import base64
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'az'))
import model as M  # noqa: E402
from buffer import Buffer  # noqa: E402


class ReplayWindowTest(unittest.TestCase):
    def test_newest_positions_across_pid_wrap_and_directories(self):
        with tempfile.TemporaryDirectory() as tmp:
            dirs = [Path(tmp) / 'a', Path(tmp) / 'b']
            for d in dirs:
                d.mkdir()
            planes = base64.b64encode(bytes(M.PACKED_BYTES)).decode()
            for name, stamp, d, values in [('game_999_0000.json', 10, dirs[0], [0, 0]),
                                           ('game_100_0000.json', 30, dirs[1], [1, 1]),
                                           ('game_800_0000.json', 20, dirs[0], [.5, .5])]:
                samples = [dict(ply=i + 4, planes=planes, progress=0,
                                policy=[[M.POLICY_SIZE - 1, .25], [70000, .75]],
                                z=z, rootq=z, a0gb=z)
                           for i, z in enumerate(values)]
                p = d / name
                p.write_text(json.dumps(dict(samples=samples)))
                os.utime(p, (stamp, stamp))
            buf = Buffer(dirs, max_positions=3)
            self.assertEqual(buf.z.tolist(), [.5, 1, 1])
            self.assertEqual(len(buf), 3)
            self.assertEqual(buf.value_targets('z', .5).tolist(),
                             [[0, 1, 0], [1, 0, 0], [1, 0, 0]])
            self.assertEqual(buf.pol_idx[0].tolist(), [M.POLICY_SIZE - 1, 70000])
            self.assertEqual(buf.packed[0].shape, (M.PACKED_BYTES,))

    def test_invalid_window(self):
        with self.assertRaises(ValueError):
            Buffer([], max_positions=0)


if __name__ == '__main__':
    unittest.main()
