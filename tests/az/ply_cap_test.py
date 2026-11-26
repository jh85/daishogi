"""daishogi-az --ply-cap: self-play stops at the cap, and the progress plane
is ply / cap. Needs ./daishogi-az (make daishogi-az)."""
import glob
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class PlyCapTest(unittest.TestCase):
    def test_cap_and_progress(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = subprocess.run(
                [str(ROOT / "daishogi-az"), "--ply-cap", "40"],
                input=f"selfplay --games 2 --sims 4 --batch 4 --seed 7 --out {tmp}\nquit\n",
                capture_output=True, text=True, timeout=300, check=True).stdout
            self.assertEqual(out.count("40 plies, plycap"), 2, out)
            for f in glob.glob(f"{tmp}/**/*.json", recursive=True):
                rec = json.loads(Path(f).read_text())
                self.assertEqual((rec["reason"], len(rec["moves"])), ("plycap", 40))
                for s in rec["samples"]:
                    self.assertAlmostEqual(s["progress"], s["ply"] / 40, places=3)

    def test_bad_cap_rejected(self):
        r = subprocess.run([str(ROOT / "daishogi-az"), "--ply-cap", "0"],
                           input="quit\n", capture_output=True, text=True, timeout=60)
        self.assertNotEqual(r.returncode, 0)


if __name__ == '__main__':
    unittest.main()
