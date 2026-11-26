"""Training-wheels schedule in run_loop.py: --material-blend and
--capture-prior at full strength until --wheels-taper-start, then linearly
to 0 over --wheels-taper-iters iterations."""
import subprocess
import sys
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'az'))
import run_loop as RL  # noqa: E402


def args(**kw):
    base = dict(material_blend=0.5, capture_prior=0.5, material_blend_scale=0.05,
                wheels_taper_start=None, wheels_taper_iters=0)
    base.update(kw)
    return Namespace(**base)


class WheelsTest(unittest.TestCase):
    def test_constant_without_taper(self):
        a = args()
        for it in (0, 175, 10_000):
            self.assertEqual(RL.wheels_at(a, it), (0.5, 0.5))

    def test_linear_taper(self):
        a = args(wheels_taper_start=200, wheels_taper_iters=50)
        self.assertEqual(RL.wheels_at(a, 150), (0.5, 0.5))
        self.assertEqual(RL.wheels_at(a, 200), (0.5, 0.5))
        self.assertEqual(RL.wheels_at(a, 225), (0.25, 0.25))
        self.assertEqual(RL.wheels_at(a, 250), (0.0, 0.0))
        self.assertEqual(RL.wheels_at(a, 400), (0.0, 0.0))
        b, p = RL.wheels_at(args(capture_prior=0.4, wheels_taper_start=0, wheels_taper_iters=4), 1)
        self.assertAlmostEqual(b, 0.375)
        self.assertAlmostEqual(p, 0.3)

    def test_options(self):
        a = args()
        self.assertEqual(RL.wheels_options(a, 0.25, 0.25),
                         " --material-blend 0.25 --material-blend-scale 0.05 --capture-prior 0.25")
        self.assertEqual(RL.wheels_options(a, 0.0, 0.25), " --capture-prior 0.25")
        self.assertEqual(RL.wheels_options(a, 0.0, 0.0), "")  # the network alone

    def test_taper_needs_length(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = subprocess.run([sys.executable, str(ROOT / 'az/run_loop.py'), '--run-dir', tmp,
                                '--material-blend', '0.5', '--wheels-taper-start', '10'],
                               capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 2)
            self.assertIn('--wheels-taper-iters', r.stderr)


if __name__ == '__main__':
    unittest.main()
