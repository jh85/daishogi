"""Puzzle and contrast records (az/make_puzzles.py): planes and policy
indices match the encoder, and an explicit value target overrides z."""
import base64
import json
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'az'))
import encode as E  # noqa: E402
import make_puzzles as P  # noqa: E402
import model as M  # noqa: E402
from buffer import Buffer  # noqa: E402

# Gote to move, so the canonical (flipped) frame is exercised; move number 301 = ply 300.
SFEN = ("laknstircosigokigosicoirstknla/rc1cs1fl1btdebt1fl1cs1rc/1vo1ab1ewphlikrew1ab1vo1/"
        "rofdsmvmbidhdkfkdkdhbivmsmfdro/papapapapapapapapapapapapapapa/4gb5gb4/15/15/15/"
        "4GB5GB4/PAPAPAPAPAPAPAPAPAPAPAPAPAPAPA/ROFDSMVMBIDHDKFKDKDHBIVMSMFDRO/"
        "1VO1AB1EWKRLIPHEW1AB1VO1/RC1CS1FL1BTDEBT1FL1CS1RC/LAKNSTIRCOSIGOKIGOSICOIRSTKNLA w - 301")


class PuzzleRecordTest(unittest.TestCase):
    def test_sample_matches_encoder(self):
        smp = P.sample_record(SFEN, ["5e5f"])
        packed = np.frombuffer(base64.b64decode(smp["planes"]), dtype=np.uint8)
        planes = M.unpack_batch(packed.tobytes(), [smp["progress"]]).numpy()[0]
        np.testing.assert_array_equal(planes, E.encode_position(E.Position(SFEN)))
        self.assertEqual(smp["ply"], 300)
        self.assertEqual(smp["stm"], 1)
        self.assertEqual(smp["policy"], [[E.move_to_index(E.flip_usi_move("5e5f")), 1.0]])

    def test_explicit_wdl_overrides_z(self):
        with tempfile.TemporaryDirectory() as tmp:
            puzzle = P.sample_record(SFEN, ["5e5f"])
            contrast = P.contrast_record(SFEN, 0.6, "5e5f")
            (Path(tmp) / "game_p.json").write_text(json.dumps(dict(samples=[puzzle, contrast])))
            got = [[round(float(v), 4) for v in r] for r in Buffer([tmp]).value_targets("z", .5)]
            self.assertEqual(sorted(got), sorted([[1.0, 0.0, 0.0], [0.2, 0.8, 0.0]]))


if __name__ == '__main__':
    unittest.main()
