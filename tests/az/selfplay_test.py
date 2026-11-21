#!/usr/bin/env python3
"""Self-play record test. Ported from ../chushogi (tests/az_selfplay_test.py,
commit d578c81).

Plays games with the uniform evaluator and validates the records:
  - JSON shape; policy probabilities sum to 1, indices are legal-move
    indices of the position (canonical frame), value targets in range
  - the moves replay legally through `daishogi batch`, and the recorded
    result/reason matches the engine's own adjudication (fourfold
    repetition, perpetual check, royal capture, no legal move; the ply cap
    is the only self-play-only ending)
  - z is the game result from the side to move's view
  - packed planes decode to exactly what the Python encoder produces from
    the independently replayed position, including the repetition planes

Usage: selfplay_test.py [--az ./daishogi-az] [--gen ./daishogi]
"""

import argparse
import base64
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "az"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np  # noqa: E402

import encode as enc  # noqa: E402
from encode_test import Cli, state  # noqa: E402

BINARY = enc.NUM_PLANES - 1


def unpack_planes(b64, progress):
    """packed binary planes + progress scalar -> (114, 225) float32."""
    raw = np.frombuffer(base64.b64decode(b64), dtype=np.uint8)
    bits = np.unpackbits(raw)
    assert len(raw) == (BINARY * enc.BOARD + 7) // 8, len(raw)
    assert not bits[BINARY * enc.BOARD:].any(), "padding bits must be zero"
    out = np.zeros((enc.NUM_PLANES, enc.BOARD), dtype=np.float32)
    out[:BINARY] = bits[:BINARY * enc.BOARD].reshape(BINARY, enc.BOARD)
    out[BINARY, :] = progress
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--az", default=str(ROOT / "daishogi-az"))
    ap.add_argument("--gen", default=str(ROOT / "daishogi"))
    ap.add_argument("--games", type=int, default=3)
    ap.add_argument("--temp-plies", type=int, default=20)
    ap.add_argument("--root-policy-temp", type=float, default=1.0)
    ap.add_argument("--full-prob", type=float, default=1.0,
                    help="playout cap randomization: only these moves are recorded")
    ap.add_argument("--mate", action="store_true",
                    help="mate solver on (root 5000 nodes, probe depth 3); a proven "
                         "win is recorded as a one-hot policy on the played move")
    a = ap.parse_args()

    az, gen = Cli([a.az]), Cli([a.gen, "batch"])
    tmp = Path(tempfile.mkdtemp(prefix="azsp_"))
    first = az.cmd(f"selfplay --games {a.games} --sims 16 --batch 4 --seed 7 "
                   f"--temp-plies {a.temp_plies} --root-policy-temp {a.root_policy_temp} "
                   f"--full-prob {a.full_prob} --fast-sims 8 --out {tmp}"
                   + (" --mate-nodes 5000 --probe-depth 3 --probe-nodes 2000" if a.mate else ""))
    assert first.startswith("game 0"), first
    for _ in range(a.games - 1):  # drain the remaining per-game lines
        az.proc.stdout.readline()

    total = failures = 0
    reasons = []
    mate_games, one_hot = [], [0]

    def fail(msg):
        nonlocal failures
        failures += 1
        print("FAIL", msg)

    paths = sorted(tmp.glob("game_*.json"))
    total += 1
    if len(paths) != a.games:
        fail(f"{len(paths)} records for {a.games} games")
    for path in paths:
        rec = json.loads(path.read_text())
        assert rec["selfplay"]["sampling_plies"] == a.temp_plies
        assert rec["selfplay"]["root_policy_temperature"] == a.root_policy_temp
        moves, initial = rec["moves"], rec["initial"]
        reasons.append(f"{rec['reason']}/{len(moves)}")

        # The moves replay legally and the engine agrees with the ending.
        total += 1
        gen.send(f"position sfen {initial}" + (" moves " + " ".join(moves) if moves else ""))
        st = gen.cmd("status").split()
        if st[0] == "error":
            fail(f"{path.name}: moves not accepted: {st}")
            continue
        if rec["reason"] == "plycap":
            ok = st == ["status", "ongoing"] and rec["result"] == 0 and len(moves) >= enc.PLY_CAP
        else:
            want = {1: ["win", "sente"], -1: ["win", "gote"], 0: ["draw"]}[rec["result"]]
            ok = st[1:] == want + [rec["reason"]]
        if not ok:
            fail(f"{path.name}: record {rec['reason']}/{rec['result']} vs engine {st}")

        total += 1
        plies = [smp["ply"] for smp in rec["samples"]]
        if a.full_prob >= 1:
            if plies != list(range(len(moves))):
                fail(f"{path.name}: {len(plies)} samples for {len(moves)} moves")
        else:  # a strictly increasing subset of about full_prob of the plies
            share = len(plies) / max(1, len(moves))
            if (plies != sorted(set(plies)) or not plies or plies[-1] >= len(moves)
                    or abs(share - a.full_prob) > 0.1):
                fail(f"{path.name}: recorded plies {len(plies)}/{len(moves)}")
        assert rec["selfplay"]["full_search_prob"] == a.full_prob

        # Real-frame repetition counts along the game.
        seen, reps_at, sfens = {}, [], []
        for n in range(len(moves) + 1):
            tail = " moves " + " ".join(moves[:n]) if n else ""
            assert az.cmd(f"position sfen {initial}{tail}") == "ok"
            sfen = az.cmd("sfen")[5:]
            sfens.append(sfen)
            reps_at.append(seen.get(state(sfen), 0))
            seen[state(sfen)] = reps_at[-1] + 1

        assert rec["selfplay"]["mate_nodes"] == (5000 if a.mate else 0)
        stats = rec["mate_stats"]
        if a.mate:
            mate_games.append(stats)
        else:
            total += 1
            if stats != {"root_mates": 0, "probes": 0, "probe_mates": 0, "solver_nodes": 0}:
                fail(f"{path.name}: solver ran while off: {stats}")
        step = max(1, len(rec["samples"]) // 15)
        checked = set(range(0, len(rec["samples"]), step)) | {
            i for i, smp in enumerate(rec["samples"]) if reps_at[smp["ply"]]} | {
            i for i, smp in enumerate(rec["samples"]) if len(smp["policy"]) == 1}
        for i in sorted(checked):
            smp = rec["samples"][i]
            ply = smp["ply"]
            pos = enc.Position(sfens[ply])
            total += 1
            probs = [p for _, p in smp["policy"]]
            idxs = [int(i) for i, _ in smp["policy"]]
            want_z = 0.5 if rec["result"] == 0 else float(
                (rec["result"] == 1) == (pos.side_to_move == 0))
            if not (abs(sum(probs) - 1.0) < 1e-4 and smp["z"] == want_z
                    and smp["stm"] == pos.side_to_move
                    and 0.0 <= smp["rootq"] <= 1.0 and 0.0 <= smp["a0gb"] <= 1.0):
                fail(f"{path.name} ply {ply}: bad targets")
                continue
            # Policy entries are exactly the legal moves (canonical frame).
            total += 1
            az.cmd(f"position sfen {sfens[ply]}")
            legal = sorted(int(x) for x in az.cmd("legal-index").split()[1:])
            played_idx = enc.move_to_index(
                enc.flip_usi_move(moves[ply]) if pos.side_to_move else moves[ply])
            if len(idxs) == 1 and len(legal) > 1:  # proven win: one-hot on the played move
                if idxs != [played_idx] or probs != [1.0] or smp["rootq"] != 1.0:
                    fail(f"{path.name} ply {ply}: bad one-hot (proven win) sample")
                one_hot[0] += 1
            elif sorted(idxs) != legal:
                fail(f"{path.name} ply {ply}: policy indices are not the legal moves")
            # The played move has visits unless sampled (only the opening).
            total += 1
            played = moves[ply]
            canon = enc.flip_usi_move(played) if pos.side_to_move else played
            if enc.move_to_index(canon) not in idxs:
                fail(f"{path.name} ply {ply}: played move {played} not in the policy")
            # Planes vs independent replay.
            total += 1
            want = enc.encode_position(pos, reps_at[ply], ply)
            got = unpack_planes(smp["planes"], smp["progress"])
            if not np.allclose(want.reshape(-1), got.reshape(-1), atol=1e-6):
                nz = np.nonzero(want.reshape(-1) != got.reshape(-1))[0]
                fail(f"{path.name} ply {ply}: plane mismatch, planes "
                     f"{sorted(set(int(d) // enc.BOARD for d in nz[:20]))}")

    az.close()
    gen.close()
    extra = ""
    if a.mate:
        total += 1
        if not any(g["probes"] for g in mate_games):
            fail("the mate solver never ran")
        extra = (f"; mate solver: {sum(g['root_mates'] for g in mate_games)} root mates, "
                 f"{sum(g['probe_mates'] for g in mate_games)} probe mates in "
                 f"{sum(g['probes'] for g in mate_games)} probes, {one_hot[0]} one-hot samples")
    print(f"{total - failures}/{total} az selfplay checks passed (games: {', '.join(reasons)}){extra}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
