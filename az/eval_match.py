#!/usr/bin/env python3
"""AZ strength evaluation: matches between an AZ player (daishogi-az `go az`,
driven by an eval server on --port) and an opponent — the random player
(`daishogi batch`, `go random`) or another AZ instance.

Ported from ../chushogi (az/eval_match.py, commit d578c81), adapted to the
`daishogi batch` line protocol. Each side receives the start position + all
moves so far and answers with its move; a third `daishogi batch` instance
validates the moves and adjudicates the game with the full rules (royal
capture, no legal move, fourfold repetition, perpetual check), plus the
PLY_CAP draw.

Usage: eval_match.py --port 51589 [--games 20] [--sims 200]
       [--port2 N] [--seed 1] [--opening-plies K]
Prints the score from the AZ player's perspective.
"""

import argparse
import json
import sys
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
INITIAL = ("laknstircosigokigosicoirstknla/rc1cs1fl1btdebt1fl1cs1rc/"
           "1vo1ab1ewphlikrew1ab1vo1/rofdsmvmbidhdkfkdkdhbivmsmfdro/"
           "papapapapapapapapapapapapapapa/4gb5gb4/15/15/15/4GB5GB4/"
           "PAPAPAPAPAPAPAPAPAPAPAPAPAPAPA/ROFDSMVMBIDHDKFKDKDHBIVMSMFDRO/"
           "1VO1AB1EWKRLIPHEW1AB1VO1/RC1CS1FL1BTDEBT1FL1CS1RC/"
           "LAKNSTIRCOSIGOKIGOSICOIRSTKNLA b - 1")
PLY_CAP = 1500  # = kPlyCap in src/az/encode.h


class Player:
    """A line-protocol engine: daishogi-az (net player, answers `position`
    with "ok") or `daishogi batch` (random player / referee; silent on
    `position`)."""

    def __init__(self, command, go, seed=None, port=None):
        command = [command] if isinstance(command, str) else list(command)
        self.batch = command[-1] == "batch"
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True)
        self.go = go
        self.port = port  # set only for net-backed players (daishogi-az)
        if seed is not None:
            self.cmd(f"seed {seed}", "ok")
        if port is not None:
            self.cmd(f"eval-server --port {port}", "ok")

    def send(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def cmd(self, line, expect):
        self.send(line)
        out = self.proc.stdout.readline().rstrip("\n")
        head, _, rest = out.partition(" ")
        if head != expect:
            raise RuntimeError(f"{line!r} -> {out!r}")
        return rest

    def position(self, initial, moves):
        line = f"position sfen {initial}" + (" moves " + " ".join(moves) if moves else "")
        if self.batch:
            self.send(line)  # errors surface as the reply to the next command
        else:
            self.cmd(line, "ok")

    def move(self, initial, moves):
        self.position(initial, moves)
        return self.cmd(self.go, "bestmove")

    def close(self):
        self.send("quit")
        self.proc.wait(timeout=10)


def random_player(binary, seed=None):
    return Player([binary, "batch"], "go random", seed=seed)


def adjudicate(ref):
    """-> None while the game goes on, else (winner 0/1 or None, reason)."""
    status = ref.cmd("status", "status").split()
    if status[0] == "ongoing":
        return None
    if status[0] == "win":
        return (0 if status[1] == "sente" else 1), status[2]
    return None, status[1]  # draw


def play_game(az, opp, ref, az_color, seed_base, opening_plies=0, div=None,
              moves=None):
    """az_color: 0 = az plays sente. Returns (1/0.5/0 from AZ's view, reason)."""
    players = [az, opp] if az_color == 0 else [opp, az]
    # Reseed the net players per game so temperature sampling differs.
    for i, p in enumerate(dict.fromkeys(players)):
        if p.port is not None:
            p.cmd(f"seed {seed_base * 1009 + i}", "ok")
    ref.position(INITIAL, [])
    moves = [] if moves is None else moves

    def result():
        end = adjudicate(ref)
        if end is None:
            return (0.5, "moveLimit") if len(moves) >= PLY_CAP else None
        winner, reason = end
        if winner is None:
            return 0.5, reason
        return (1.0 if winner == az_color else 0.0), reason

    # Optional opening diversification: `div` plays random moves first, so
    # deterministic AZ-vs-AZ pairings don't replay one line every game.
    for _ in range(opening_plies):
        moves.append(div.move(INITIAL, moves))
        ref.cmd(f"move {moves[-1]}", "ok")
        done = result()
        if done:
            return done
    while True:
        side = len(moves) % 2
        best = players[side].move(INITIAL, moves)
        if best in ("resign", "none"):
            winner = 1 - side
            return (1.0 if winner == az_color else 0.0), "resign"
        ref.cmd(f"move {best}", "ok")
        moves.append(best)
        done = result()
        if done:
            return done


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--az", default=str(ROOT / "daishogi-az"))
    ap.add_argument("--gen", default=str(ROOT / "daishogi"),
                    help="rules engine: random player and referee (`daishogi batch`)")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--port2", type=int, default=None)
    ap.add_argument("--games", type=int, default=20)
    ap.add_argument("--sims", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--paired-openings", action="store_true",
                    help="reseed random openings identically for each color pair")
    ap.add_argument("--json-out", default="", help="save scores and full move traces")
    ap.add_argument("--opening-plies", type=int, default=0,
                    help="random opening plies before the engines take over "
                         "(diversify deterministic AZ-vs-AZ matchups)")
    ap.add_argument("--temp-plies", type=int, default=0,
                    help="sample AZ moves from the visit distribution for "
                         "this many opening plies (noisy match play)")
    ap.add_argument("--mate-nodes", type=int, default=5000, help="root mate budget; 0 disables")
    ap.add_argument("--probe-depth", type=int, default=3, help="in-tree mate depth; 0 disables")
    ap.add_argument("--probe-nodes", type=int, default=2000)
    a = ap.parse_args()
    if a.games <= 0 or (a.paired_openings and a.games % 2):
        ap.error("games must be positive and even for paired openings")

    extra = f" --temp-plies {a.temp_plies}" if a.temp_plies else ""
    extra += (f" --mate-nodes {a.mate_nodes} --probe-depth {a.probe_depth}"
              f" --probe-nodes {a.probe_nodes}")
    az = Player(a.az, f"go az --sims {a.sims}{extra}", port=a.port)
    if a.port2 is not None:
        opp = Player(a.az, f"go az --sims {a.sims}{extra}", port=a.port2)
        opp_name = f"az@{a.port2}"
    else:
        opp = random_player(a.gen, seed=a.seed)
        opp_name = "random"
    ref = random_player(a.gen)  # referee; never asked for moves
    div = random_player(a.gen, seed=a.seed * 1000003 + 7) if a.opening_plies else None
    players = [p for p in (az, opp, ref, div) if p is not None]

    score = 0.0
    counts = {}
    records = []
    try:
        for g in range(a.games):
            az_color = g % 2
            if div and a.paired_openings:
                div.cmd(f"seed {a.seed * 1000003 + g // 2}", "ok")
            moves = []
            pts, reason = play_game(az, opp, ref, az_color, a.seed + g,
                                    a.opening_plies, div, moves)
            records.append(dict(game=g, az_color=az_color, score=pts,
                                reason=reason, moves=moves))
            if a.json_out:
                Path(a.json_out).write_text(json.dumps(
                    dict(config=vars(a), games=records), indent=2) + "\n")
            score += pts
            counts[reason] = counts.get(reason, 0) + 1
            print(f"game {g}: {'win' if pts == 1 else 'draw' if pts else 'loss'}"
                  f" ({reason}, {len(moves)} plies)", flush=True)
    finally:
        for p in players:
            p.close()
    n = a.games
    print(f"AZ vs {opp_name}: {score}/{n} = {100 * score / n:.1f}%  {counts}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
