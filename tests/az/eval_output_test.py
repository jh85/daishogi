"""The evaluator must preserve fp16 wire outputs, including edge values.
Ported from ../chushogi (tests/az_eval_output_test.py, commit d578c81)."""
import sys
import unittest
from pathlib import Path

import numpy as np
import torch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'az'))
import model as M  # noqa: E402
from eval_server import Server, PACKED  # noqa: E402

N = M.POLICY_SIZE


class OutputTest(unittest.TestCase):
    def test_wire_values_shapes_and_signed_zero(self):
        generator = torch.Generator().manual_seed(123)
        source = torch.randn(2, N, generator=generator) * 10
        source[0, :9] = torch.tensor([0., -0., 2**-25, 2**-24, 2**-14, 65504., 70000.,
                                      float('inf'), -float('inf')])
        for device in ['cpu'] + (['cuda'] if torch.cuda.is_available() else []):
            for dtype in [torch.float32, torch.bfloat16]:
                with self.subTest(device=device, dtype=dtype):
                    policy = source.to(device=device, dtype=dtype)
                    wdl = torch.tensor([[.1234, -.00001, 5.], [1., -2., 3.]],
                                       device=device, dtype=dtype)
                    srv = Server.__new__(Server)
                    srv.device = device
                    srv.dtype = torch.bfloat16
                    srv.profile_eval = False
                    srv.net = lambda planes: (policy, wdl)
                    actual = srv.eval(bytes(2 * PACKED), np.zeros(2, dtype=np.float32))
                    with np.errstate(over='ignore', invalid='ignore'):
                        reference = (policy.float().cpu().numpy().astype(np.float16),
                                     wdl.float().cpu().numpy().astype(np.float16))
                    indices = [np.array([N - 1, 70000, 0, 1, 2, 3, 4, 5, 6, 7, 8, 3]),
                               np.array([2, 0])]
                    sparse, sparse_wdl = srv.eval(bytes(2 * PACKED),
                                                  np.zeros(2, dtype=np.float32), indices)
                    for row, requested in enumerate(indices):
                        self.assertEqual(sparse[row, :len(requested)].tobytes(),
                                         reference[0][row, requested].tobytes())
                    self.assertEqual(sparse_wdl.tobytes(), reference[1].tobytes())
                    for got, want in zip(actual, reference):
                        self.assertEqual(got.dtype, np.dtype('float16'))
                        self.assertEqual(got.shape, want.shape)
                        self.assertEqual(got.tobytes(), want.tobytes())


if __name__ == '__main__':
    unittest.main()
