"""Material scoring of capped games (az/material.py) and the value targets
the replay buffer derives from it."""
import base64
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'az'))
import encode as E  # noqa: E402
import material as MT  # noqa: E402
import model as M  # noqa: E402
from buffer import Buffer  # noqa: E402

INITIAL = ("laknstircosigokigosicoirstknla/rc1cs1fl1btdebt1fl1cs1rc/"
           "1vo1ab1ewphlikrew1ab1vo1/rofdsmvmbidhdkfkdkdhbivmsmfdro/"
           "papapapapapapapapapapapapapapa/4gb5gb4/15/15/15/4GB5GB4/"
           "PAPAPAPAPAPAPAPAPAPAPAPAPAPAPA/ROFDSMVMBIDHDKFKDKDHBIVMSMFDRO/"
           "1VO1AB1EWKRLIPHEW1AB1VO1/RC1CS1FL1BTDEBT1FL1CS1RC/"
           "LAKNSTIRCOSIGOKIGOSICOIRSTKNLA b - 1")
# The initial position without Gote's lion (rank 3, the "li" of "ewphlikrew").
NO_GOTE_LION = INITIAL.replace("1vo1ab1ewphlikrew1ab1vo1", "1vo1ab1ewph1krew1ab1vo1")


class MaterialTest(unittest.TestCase):
    def test_table_covers_every_kind(self):
        self.assertEqual(len(MT.VALUES), E.KINDS)
        self.assertTrue(all(v > 0 for v in MT.VALUES))
        self.assertEqual(MT.VALUES[E.KIND_INDEX['LI']], 1000)
        self.assertEqual(MT.VALUES[E.KIND_INDEX['+KR']], 1000)  # lion from kirin
        self.assertEqual(MT.VALUES[E.KIND_INDEX['+PA']], MT.VALUES[E.KIND_INDEX['GO']] )

    def test_initial_position_is_even(self):
        s, g = MT.material(INITIAL)
        self.assertEqual(s, g)
        self.assertEqual(MT.cap_score(INITIAL, INITIAL), 0.0)

    def test_score_sign_bound_and_symmetry(self):
        s, g = MT.material(NO_GOTE_LION)
        self.assertEqual(s - g, 1000)
        t = MT.cap_score(INITIAL, NO_GOTE_LION)
        army = MT.material(INITIAL)[0]
        self.assertAlmostEqual(t, 0.5 * math.tanh(1000 / army / 0.1), places=6)
        self.assertTrue(0 < t < 0.5)
        flipped = E.Position(NO_GOTE_LION).flipped()  # Sente is now the one short
        self.assertEqual(MT.material(_sfen(flipped)), (g, s))
        self.assertAlmostEqual(MT.cap_score(INITIAL, _sfen(flipped)), -t, places=6)
        self.assertEqual(MT.wdl(0.3), [0.3, 0.7, 0.0])
        self.assertEqual(MT.wdl(-0.3), [0.0, 0.7, 0.3])


def _sfen(pos):
    """Board-only SFEN of an encode.Position (enough for material())."""
    ranks = []
    for y in range(E.RANKS):
        row, empty = "", 0
        for x in range(E.FILES):
            pc = pos.board[y * E.FILES + x]
            if pc is None:
                empty += 1
                continue
            if empty:
                row, empty = row + str(empty), 0
            code = E.KIND_CODES[pc[0]]
            row += code if pc[1] == 0 else code.lower()
        ranks.append(row + (str(empty) if empty else ""))
    return "/".join(ranks) + f" {pos.side} - 1"


class BufferCapScoreTest(unittest.TestCase):
    def _write(self, d, name, reason, result, z, final):
        planes = base64.b64encode(bytes(M.PACKED_BYTES)).decode()
        samples = [dict(ply=10 + i, stm=i % 2, planes=planes, progress=0,
                        policy=[[0, 1.0]], z=z[i % 2], rootq=.5, a0gb=.5)
                   for i in range(2)]
        rec = dict(initial=INITIAL, reason=reason, result=result, samples=samples)
        if final:
            rec["final"] = final
        (d / name).write_text(json.dumps(rec))

    def test_targets(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self._write(d, "game_1_0000.json", "plycap", 0, [.5, .5], NO_GOTE_LION)
            self._write(d, "game_2_0000.json", "plycap", 0, [.5, .5], None)  # old record
            self._write(d, "game_3_0000.json", "royals", 1, [1.0, 0.0], NO_GOTE_LION)
            t = MT.cap_score(INITIAL, NO_GOTE_LION)
            buf = Buffer([d], cap_score_max=0.5)
            got = {}
            for row, z, cap in zip(buf.value_targets('z', .5), buf.z, buf.cap_t):
                got.setdefault((float(z), bool(math.isnan(cap))), []).append(
                    [round(float(v), 4) for v in row])
            want_capped = sorted([[round(t, 4), round(1 - t, 4), 0.0],     # Sente to move
                                  [0.0, round(1 - t, 4), round(t, 4)]])    # Gote to move
            self.assertEqual(sorted(got[(0.5, False)]), want_capped)
            self.assertEqual(got[(0.5, True)], [[0, 1, 0], [0, 1, 0]])     # no final: draw
            self.assertEqual(sorted(got[(1.0, True)] + got[(0.0, True)]),
                             [[0, 0, 1], [1, 0, 0]])                       # wins untouched
            off = Buffer([d])
            self.assertTrue(all(r.tolist() == [0, 1, 0]
                                for r, z in zip(off.value_targets('z', .5), off.z) if z == .5))
            with self.assertRaises(ValueError):
                buf.value_targets('blend', .5)


if __name__ == '__main__':
    unittest.main()
