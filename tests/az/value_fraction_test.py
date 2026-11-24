"""--value-fraction: which batch rows get the value loss (az/train.py)."""
import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'az'))
from train import value_rows  # noqa: E402


class ValueRowsTest(unittest.TestCase):
    def test_full_fraction_keeps_every_row(self):
        m = value_rows(np.random.default_rng(0), 200, 256, 1.0)
        self.assertTrue((m == 1).all())

    def test_share_of_game_rows_and_all_puzzle_rows(self):
        rng = np.random.default_rng(1)
        ms = np.stack([value_rows(rng, 230, 256, 0.1) for _ in range(400)])
        self.assertTrue((ms[:, 230:] == 1).all())               # puzzle rows always
        self.assertTrue(set(np.unique(ms[:, :230])) <= {0.0, 1.0})
        self.assertAlmostEqual(ms[:, :230].mean(), 0.1, delta=0.005)

    def test_zero_fraction(self):
        m = value_rows(np.random.default_rng(2), 256, 256, 0.0)
        self.assertEqual(m.sum(), 0)


if __name__ == '__main__':
    unittest.main()
