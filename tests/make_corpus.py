#!/usr/bin/env python3
"""Builds the reference test corpus (tests/data/*.jsonl) from haitaka-variants.

Every position's expected legal-move list comes from the reference
implementation (tools/refgen, Shogitter rule 49), converted to our notation
by refconv.py. Sources:

  games.jsonl      positions sampled from reference-played random games
                   (uniform and capture-biased), plus one "game" record per
                   game (start, moves, final position, reference outcome)
  pieces.jsonl     every piece kind of both colors on several squares, alone
                   and with random surroundings
  scatter.jsonl    random piece placements (dense lion/royal interactions,
                   random lion-capture state, both sides to move)
  targeted.jsonl   hand-written rule cases and their 180-degree mirrors
  rule_games.jsonl scripted games for game-level rules (repetition draw,
                   perpetual check, prince after king capture, no legal
                   move), with the reference's result and final position

Position records carry "expected" (legal moves: the reference's moves minus
the ones our rules deliberately forbid, listed as "reference_only", plus the
ones they deliberately add, listed as "ours_only"; see tools/refgen and
docs/RULES.md), "outcome" (the reference's
result for that position taken as a fresh game) and "next": for every move
(or, for game positions, every capture / promotion / two-step move) a short
digest of the position after it, so move application and the lion-capture
state are compared too. All files are gzip-compressed JSON lines.

Usage: make_corpus.py --refgen PATH --out DIR [--seed N] [--scale F]
"""

import argparse
import json
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from refconv import (CODE_TO_REF, DEAD_RANKS, INITIAL_COUNT,  # noqa: E402
                     base_code, make_sfen, parse_sfen, parse_square,
                     ref_move_to_usi, ref_outcome_token, sfen_to_wire,
                     usi_to_ref_move, wire_state_hash, wire_to_sfen)


# ----------------------------------------------------------- reference I/O

def nontrivial(pieces, usi):
    """Capture, promotion or two-step move (pieces: parse_sfen()[0])."""
    if usi.endswith("+") or sum(ch.isalpha() for ch in usi) == 3:
        return True
    body = usi.rstrip("+")
    i = len(body) - 1
    while i > 0 and body[i - 1].isdigit():
        i -= 1
    return parse_square(body[i:]) in pieces


def our_moves(d):
    """Expected moves from a refgen record: the reference's moves minus the
    ones our rules forbid plus the ones they add (tools/refgen, docs/RULES.md
    discrepancies 5 and 9). Both sets are kept in the record for reference."""
    forbidden = set(ref_move_to_usi(m) for m in d["forbidden"])
    added = set(ref_move_to_usi(m) for m in d["added"])
    r = {"expected": sorted((set(ref_move_to_usi(m) for m in d["moves"]) - forbidden) | added)}
    if forbidden:
        r["reference_only"] = sorted(forbidden)
    if added:
        r["ours_only"] = sorted(added)
    return r


def _ref_moves_chunk(args):
    refgen, sfens, transitions = args
    cmd = [refgen, "moves"] + (["--transitions"] if transitions != "none" else [])
    inp = "".join(json.dumps(sfen_to_wire(s)) + "\n" for s in sfens)
    out = subprocess.run(cmd, input=inp, capture_output=True, text=True,
                         check=True).stdout.splitlines()
    assert len(out) == len(sfens)
    res = []
    for sfen, line in zip(sfens, out):
        d = json.loads(line)
        if "error" in d:
            res.append(d["error"])
            continue
        r = our_moves(d)
        r["outcome"] = ref_outcome_token(d["outcome"])
        if transitions != "none":
            nxt = {}
            pieces = parse_sfen(sfen)[0]
            dev = set(r.get("reference_only", []))
            for m, w in list(zip(d["moves"], d["next"])) + list(zip(d["added"], d["added_next"])):
                u = ref_move_to_usi(m)
                if u in dev:
                    continue
                if transitions == "all" or nontrivial(pieces, u):
                    nxt[u] = wire_state_hash(w, m)
            r["next"] = nxt
        res.append(r)
    return res


def ref_moves(refgen, sfens, transitions="all"):
    """Reference result for each SFEN: a record fragment, or an error string.
    transitions: "all", "nontrivial" or "none". Runs in parallel."""
    from multiprocessing import Pool
    size = 64
    chunks = [(refgen, sfens[i:i + size], transitions) for i in range(0, len(sfens), size)]
    with Pool(min(16, os.cpu_count() or 1)) as pool:
        return [r for part in pool.map(_ref_moves_chunk, chunks) for r in part]


def ref_replay(refgen, games):
    """games: [(start sfen, [usi])] -> reference replay results."""
    inp = "".join(json.dumps({"start": sfen_to_wire(s),
                              "moves": [usi_to_ref_move(u) for u in mv]}) + "\n"
                  for s, mv in games)
    out = subprocess.run([refgen, "replay"], input=inp, capture_output=True,
                         text=True, check=True).stdout.splitlines()
    return [json.loads(line) for line in out]


def ref_games(refgen, seed, games, max_plies, sample, bias, tag):
    """Positions and game records from reference-played games."""
    cmd = [refgen, "games", "--seed", str(seed), "--games", str(games),
           "--max-plies", str(max_plies), "--sample", str(sample),
           "--capture-bias", str(bias)]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
    positions, records = [], []
    for line in proc.stdout.splitlines():
        d = json.loads(line)
        if d["type"] == "position":
            positions.append({
                "id": f"{tag}-s{d['seed']}-p{d['ply']}",
                "sfen": wire_to_sfen(d["position"], d["lastMove"]),
                **our_moves(d),
                "outcome": ref_outcome_token(d["outcome"]),
            })
        else:
            records.append({
                "type": "game", "id": f"{tag}-s{d['seed']}",
                "start": START_SFEN,
                "moves": [ref_move_to_usi(m) for m in d["moves"]],
                "outcome": ref_outcome_token(d["outcome"], d["cutoff"]),
                "final": None,  # filled from the last position record
                "plies": d["plies"],
            })
            records[-1]["final"] = positions[-1]["sfen"]
    return positions, records


START_SFEN = ("laknstircosigokigosicoirstknla/rc1cs1fl1btdebt1fl1cs1rc/1vo1ab1ewphlikrew1ab1vo1/"
              "rofdsmvmbidhdkfkdkdhbivmsmfdro/papapapapapapapapapapapapapapa/"
              "4gb5gb4/15/15/15/4GB5GB4/"
              "PAPAPAPAPAPAPAPAPAPAPAPAPAPAPA/ROFDSMVMBIDHDKFKDKDHBIVMSMFDRO/"
              "1VO1AB1EWKRLIPHEW1AB1VO1/RC1CS1FL1BTDEBT1FL1CS1RC/LAKNSTIRCOSIGOKIGOSICOIRSTKNLA b - 1")


# ------------------------------------------------------- position builders

def build(spec, side="b", lion=None):
    """spec: {"8h": "LI", "8f": "li", "7c": "+dh"}; case gives the color."""
    pieces = {}
    for sq, code in spec.items():
        color = 1 if code.lower() == code else 0
        pieces[parse_square(sq)] = (code.upper(), color)
    return make_sfen(pieces, 0 if side == "b" else 1,
                     parse_square(lion) if lion else None)


def mirror(sfen):
    """Rotate 180 degrees and swap colors and side to move."""
    pieces, side, lion = parse_sfen(sfen)
    m = {(16 - x, 16 - y): (c, 1 - col) for (x, y), (c, col) in pieces.items()}
    return make_sfen(m, 1 - side, (16 - lion[0], 16 - lion[1]) if lion else None)


class Inventory:
    """Per-side piece budget, as the reference validates it."""

    def __init__(self):
        self.left = [dict(INITIAL_COUNT), dict(INITIAL_COUNT)]

    def take(self, code, color):
        b = base_code(code)
        if self.left[color][b] <= 0:
            return False
        self.left[color][b] -= 1
        return True


def all_codes():
    return list(CODE_TO_REF)


def legal_square(code, color, x, y):
    rel = y - 1 if color == 0 else 15 - y
    return rel >= DEAD_RANKS.get(code, 0)


def scatter(rng, n_pieces, extra=None, inv=None, kings=True, lion_prob=0.15,
            codes=None):
    """Random position: kings (optional) + n random pieces."""
    inv = inv or Inventory()
    pieces = dict(extra or {})
    for sq, (code, color) in pieces.items():
        inv.take(code, color)
    free = [(x, y) for x in range(1, 16) for y in range(1, 16) if (x, y) not in pieces]
    rng.shuffle(free)
    if kings:
        for color in (0, 1):
            if any(c == "KI" and col == color for c, col in pieces.values()):
                continue
            if inv.take("KI", color):
                pieces[free.pop()] = ("KI", color)
    codes = codes or all_codes()
    tries = 0
    while n_pieces > 0 and free and tries < 10000:
        tries += 1
        code = rng.choice(codes)
        color = rng.randrange(2)
        x, y = free[-1]
        if not legal_square(code, color, x, y) or not inv.take(code, color):
            continue
        pieces[free.pop()] = (code, color)
        n_pieces -= 1
    side = rng.randrange(2)
    lion = None
    if rng.random() < lion_prob:
        lion = rng.choice(list(pieces))
    return make_sfen(pieces, side, lion)


# ------------------------------------------------------- hand-written cases

def targeted_cases():
    K = {"15o": "KI", "1a": "ki"}  # kings out of the way
    c = []

    def add(name, spec, side="b", lion=None, kings=True):
        s = dict(K) if kings else {}
        s.update(spec)
        s = {k: v for k, v in s.items() if v is not None}
        c.append((name, build(s, side, lion)))

    # --- lion versus lion
    add("lion-takes-adjacent-protected-lion", {"8h": "LI", "8g": "li", "8f": "go"})
    add("lion-jump-onto-protected-lion", {"8h": "LI", "8f": "li", "8e": "go"})
    add("lion-jump-onto-unprotected-lion", {"8h": "LI", "8f": "li"})
    add("lion-diagonal-jump-protected", {"8h": "LI", "6f": "+kr", "5e": "si"})
    add("lion-protection-xray-through-capturer", {"8h": "LI", "8f": "li", "8k": "rc"})
    add("lion-protection-xray-blocked", {"8h": "LI", "8f": "li", "8k": "rc", "8j": "PA"})
    add("lion-protected-by-lion-jump", {"8h": "LI", "8f": "li", "8d": "+kr"})
    add("tsukegui-substantial-first-capture", {"8h": "LI", "8g": "go", "8f": "li", "8e": "go"})
    add("tsukegui-pawn-first-capture", {"8h": "LI", "8g": "pa", "8f": "li", "8e": "go"})
    add("tsukegui-go-between-first-capture", {"8h": "LI", "8g": "gb", "8f": "li", "8e": "go"})
    add("tsukegui-pawn-blocks-protector", {"8h": "LI", "7g": "pa", "6f": "li", "10j": "bi"})
    # pawn / go-between taken first: it stops blocking (deliberate difference
    # from the reference, which lists these moves as "reference_only") but
    # still protects itself (JCSA 2004 supplement, case 4)
    add("tsukegui-go-between-blocks-protector", {"8h": "LI", "7g": "gb", "6f": "li", "10j": "bi"})
    add("tsukegui-pawn-off-protector-line", {"8h": "LI", "7f": "pa", "6f": "li", "10j": "bi"})
    add("pawn-then-adjacent-lion-pawn-blocks-protector", {"8h": "LI", "8g": "pa", "7h": "li", "10e": "bi"})
    add("go-between-is-only-protector", {"8h": "LI", "8g": "gb", "8f": "li"})
    add("pawn-is-only-protector", {"8i": "LI", "8h": "pa", "8g": "li"})
    # the protection examples from the rule discussion (足 and かげ足)
    add("protected-lion-example", {"6e": "LI", "6i": "RO", "4d": "li", "1a": "bi", "15a": "ki"})
    add("kage-ashi-example", {"7g": "bi", "1a": "li", "3c": "LI", "3i": "RO", "15a": "ki"})
    add("lion-protector-xray-diagonal", {"8h": "LI", "6f": "li", "10j": "bi"})
    add("lion-two-step-to-adjacent-protected-lion", {"8h": "LI", "7h": "li", "6h": "go"})
    add("lion-takes-pawn-then-adjacent-protected-lion", {"8h": "LI", "8g": "pa", "7h": "li", "6h": "go"})
    add("lion-double-lion-capture", {"8h": "LI", "8g": "li", "8f": "+kr", "8e": "go"})
    add("lion-igui-and-pass", {"8h": "LI", "8g": "si", "7g": "GO", "9g": "GO"})
    add("lion-boxed-in", {"8h": "LI", "7g": "GO", "8g": "GO", "9g": "SI", "7h": "SI",
                          "9h": "CO", "7i": "CO", "8i": "BT", "9i": "BT"})
    add("lion-in-corner", {"1o": "LI", "2n": "pa", "15o": "KI"}, kings=True)
    add("lion-from-kirin-versus-lion", {"8h": "+KR", "8f": "li", "8e": "go"})
    # --- the 先獅子 (counter-strike) restriction
    add("senjishi-blocks-non-lion-captures",
        {"8f": "li", "8l": "RO", "8h": "KR", "7g": "SI", "6f": "LI"}, lion="2c")
    add("no-senjishi-non-lion-captures",
        {"8f": "li", "8l": "RO", "8h": "KR", "7g": "SI", "6f": "LI"})
    add("senjishi-lion-may-still-capture", {"8f": "li", "8h": "LI", "8e": "go"}, lion="2c")
    add("senjishi-kirin-capture-and-promote", {"8f": "KR", "8d": "li"}, lion="5e")
    add("kirin-captures-lion-and-may-promote", {"8f": "KR", "8d": "li"})
    # --- horned falcon / soaring eagle lion power
    add("falcon-captures-lion-first-step", {"8h": "+DH", "8g": "li"})
    add("falcon-captures-lion-second-step", {"8h": "+DH", "8f": "li"})
    add("falcon-two-lions", {"8h": "+DH", "8g": "li", "8f": "+kr"})
    add("falcon-senjishi-first-step", {"8h": "+DH", "8g": "li"}, lion="2c")
    add("falcon-senjishi-second-step", {"8h": "+DH", "8f": "li"}, lion="2c")
    add("falcon-senjishi-pawn-then-lion", {"8h": "+DH", "8g": "pa", "8f": "li"}, lion="2c")
    add("falcon-blocked-by-own", {"8h": "+DH", "8g": "PA", "8f": "go"})
    add("eagle-lion-power", {"8h": "+DK", "9g": "li", "10f": "go", "7g": "pa", "6f": "+kr"})
    add("eagle-senjishi", {"8h": "+DK", "9g": "li", "6f": "+kr"}, lion="2c")
    # --- royal capture
    add("lion-first-step-takes-last-king", {"8h": "LI", "8g": "ki", "1a": None, "15o": "KI"})
    add("lion-first-step-takes-king-prince-remains", {"8h": "LI", "8g": "ki", "3c": "+de", "1a": None})
    add("only-prince-left", {"8h": "+de", "1a": None, "7i": "GO"}, side="w")
    add("gote-has-no-royals", {"1a": None, "5e": "go"})
    add("sente-has-no-royals", {"15o": None, "5e": "GO"}, side="w")
    add("falcon-first-step-takes-last-king", {"8h": "+DH", "8g": "ki", "1a": None})
    # --- promotion
    add("promotion-entering-zone", {"8f": "SI", "6f": "GO", "4f": "+SI"})
    add("promotion-within-zone", {"8d": "SI", "6c": "BI", "3e": "RO"})
    add("promotion-leaving-zone", {"8e": "SI", "6e": "CO", "4e": "DE"})
    add("no-promotion-outside-zone", {"8g": "SI", "6h": "RO", "4g": "BT"})
    add("forced-promotion-pawn-lance", {"8b": "PA", "6c": "LA", "4b": "LA"})
    add("forced-promotion-knight", {"8c": "KN", "5d": "KN"})
    add("knight-optional-promotion", {"8e": "KN", "4g": "KN"})
    add("forced-promotion-iron-stone", {"8b": "IR", "5b": "ST", "11c": "IR"})
    add("promoted-pieces-do-not-promote", {"8c": "+PA", "7c": "+KN", "6c": "+BI", "5c": "+KR", "4c": "+DH"})
    add("capture-promotions", {"8e": "DK", "8b": "go", "5e": "DH", "3c": "si", "11f": "PH", "9d": "pa"})
    # --- file restrictions (二歩・二仲人)
    add("two-pawns-on-a-file", {"3i": "PA", "3k": "PA", "3c": "PA", "3m": "PA"}, kings=True)
    add("two-go-betweens-on-a-file", {"5i": "GB", "5k": "GB", "5c": "gb", "5e": "gb"})
    add("go-between-file-limit-in-zone", {"5d": "GB", "5k": "GB"})
    add("pawn-file-limit-ignores-promoted-and-enemy", {"3i": "PA", "3k": "+PA", "3h": "pa", "3g": "pa"})
    # --- limited-range sliders
    add("violent-ox-range-two", {"8h": "VO", "8f": "go", "6h": "PA", "10h": "pa"})
    add("flying-dragon-range-two", {"8h": "FD", "6f": "go", "9g": "PA", "10j": "pa"})
    add("range-two-at-edge", {"1o": "VO", "2n": "FD"}, kings=True)
    # --- jumping pieces ignore blockers
    add("kirin-phoenix-jumps", {"8h": "KR", "8g": "pa", "8f": "pa", "7h": "PH", "6f": "go", "6g": "PA"})
    add("knight-jumps-over", {"8h": "KN", "8g": "PA", "7f": "pa", "9f": "PA"})
    # --- dense middle game around both lions
    add("lion-melee", {"8h": "LI", "8g": "li", "7g": "pa", "9g": "go", "7h": "+kr", "6h": "gb",
                       "8f": "si", "9i": "+DH", "7i": "+DK", "8j": "rc", "10f": "bi"})
    # mirrors: same cases with Gote to move
    return c + [(name + "-mirror", mirror(s)) for name, s in c]


def piece_cases(rng):
    """Every kind, both colors, on several squares; alone and cluttered."""
    out = []
    squares = ["8h", "8f", "8e", "8c", "8b", "1h", "15e", "2b", "14n"]
    for code in all_codes():
        for color in (0, 1):
            for sq in squares:
                x, y = parse_square(sq)
                if color == 1:
                    x, y = 16 - x, 16 - y
                if not legal_square(code, color, x, y):
                    continue
                pc = {(x, y): (code, color)}
                kings = {(15, 15): ("KI", 0), (1, 1): ("KI", 1)}
                if (x, y) in kings:
                    continue
                if code == "KI":
                    kings = {k: v for k, v in kings.items() if v[1] != color}
                alone = dict(kings)
                alone.update(pc)
                out.append((f"piece-{code}-{color}-{sq}", make_sfen(alone, color)))
                for k in range(2):
                    s = scatter(rng, rng.choice([10, 25, 45]), extra=pc)
                    p, _, lion = parse_sfen(s)
                    out.append((f"piece-{code}-{color}-{sq}-clutter{k}", make_sfen(p, color, lion)))
    return out


def scatter_cases(rng, n):
    out = []
    lions = ["LI", "+KR", "+DH", "+DK", "KR", "KI", "+DE"]
    for i in range(n):
        r = rng.random()
        if r < 0.4:
            # dense around lions: draw most pieces from lion-related kinds
            s = scatter(rng, rng.randrange(8, 40), codes=lions * 3 + list(CODE_TO_REF),
                        lion_prob=0.3)
        else:
            s = scatter(rng, rng.randrange(5, 110), lion_prob=0.2)
        if rng.random() < 0.05:
            # occasionally no royal for one side (game over)
            p, side, lion = parse_sfen(s)
            col = rng.randrange(2)
            p = {k: v for k, v in p.items() if not (v[0] in ("KI", "+DE") and v[1] == col)}
            if any(v[0] in ("KI", "+DE") for v in p.values()):
                s = make_sfen(p, side, lion)
        out.append((f"scatter-{i}", s))
    return out


# ------------------------------------------------------- scripted games

def rule_games():
    """(name, start sfen, [usi moves]) exercising game-level rules."""
    K = {"15o": "KI", "1a": "ki"}
    g = []
    shuffle = ["1n1m", "15b15c", "1m1n", "15c15b"]
    g.append(("repetition-draw-fourth-occurrence", START_SFEN, shuffle * 3))
    g.append(("repetition-third-occurrence-continues", START_SFEN, shuffle * 2 + shuffle[:3]))
    # Sente's rook checks on every move (1h <-> 1i on the king's file).
    checks = ["1h1i", "5a5b", "1i1h", "5b5a"]
    perp = build({**K, "1h": "RO", "5a": "go"})
    g.append(("perpetual-check-checked-side-wins", perp, checks * 3))
    g.append(("perpetual-check-checked-side-wins-mirror", mirror(perp),
              ["15h15i", "11o11n", "15i15h", "11n11o"] * 3))
    # Checks only every other move: plain repetition draw.
    some = ["1h2h", "5a5b", "2h1h", "5b5a"]
    g.append(("repetition-with-intermittent-checks-draws", perp, some * 3))
    # Both lions pass: the position repeats every two plies.
    lions = build({**K, "8h": "LI", "8d": "li"})
    g.append(("lion-pass-repetition", lions, ["8h8g8h", "8d8e8d"] * 3))
    # Taking the king does not end the game while the prince lives.
    pr = build({**K, "8h": "LI", "8g": "ki", "8e": "+de", "1e": "pa", "1a": "si"})
    g.append(("king-captured-prince-remains", pr, ["8h8g"]))
    g.append(("king-then-prince-captured", pr, ["8h8g", "1e1f", "8g8f8e"]))
    g.append(("royal-capture-by-igui", build({**K, "8h": "LI", "8g": "ki", "1a": "si"}), ["8h8g8h"]))
    # Sente's king and pieces are boxed in by their own pieces.
    g.append(("no-legal-move-loses", build({"1a": "KI", "1b": "PA", "2a": "RC", "2b": "PA", "15o": "ki"}), []))
    # A falcon takes a lion. If it stops there, Gote's rook may not take
    # Sente's lion next (先獅子). If it steps on, the reference derives the
    # restriction from the last hop only, so the rook capture is legal.
    fal = build({**K, "8h": "+DH", "8g": "li", "3h": "LI", "3c": "ro"})
    g.append(("falcon-takes-lion-and-stops-then-rook-takes-lion", fal, ["8h8g", "3c3h"]))
    g.append(("falcon-takes-lion-moves-on-then-rook-takes-lion", fal, ["8h8g8f", "3c3h"]))
    g.append(("falcon-igui-lion-then-rook-takes-lion", fal, ["8h8g8h", "3c3h"]))
    g.append(("non-lion-may-take-lion-after-one-quiet-move", fal, ["8h8g", "1a2a", "15o14o", "3c3h"]))
    return g


def rule_game_records(refgen):
    games = rule_games()
    res = ref_replay(refgen, [(s, m) for _, s, m in games])
    out = []
    for (name, start, moves), r in zip(games, res):
        if not r["ok"]:
            # The reference rejected a move: record that it must be illegal.
            out.append({"type": "game", "id": name, "start": start, "moves": moves,
                        "illegal_at": r["plies"], "outcome": None, "final": None})
            continue
        last = usi_to_ref_move(moves[-1]) if moves else None
        out.append({"type": "game", "id": name, "start": start, "moves": moves,
                    "outcome": ref_outcome_token(r["outcome"]),
                    "final": wire_to_sfen(r["final"], last)})
    return out


# ------------------------------------------------------------------- main

GAME_CONFIGS = [
    # (tag, seed, games, max plies, sample every, capture bias)
    ("uniform", 101, 40, 3000, 12, 0.0),
    ("capture30", 202, 40, 3000, 8, 0.3),
    ("capture80", 303, 40, 3000, 6, 0.8),
]


def evaluate(refgen, named, transitions="all"):
    results = ref_moves(refgen, [s for _, s in named], transitions)
    records, rejected = [], []
    for (name, sfen), res in zip(named, results):
        if isinstance(res, str):
            rejected.append((name, res))
            continue
        records.append({"id": name, "sfen": sfen, **res})
    for name, err in rejected:
        print(f"  reference rejected {name}: {err}", file=sys.stderr)
    return records


def write(path, records):
    import gzip
    import io
    # mtime=0 keeps the compressed files byte-for-byte reproducible
    with io.TextIOWrapper(gzip.GzipFile(path, "wb", 9, mtime=0)) as f:
        for r in records:
            f.write(json.dumps(r, separators=(",", ":")) + "\n")
    print(f"wrote {len(records)} records to {path}")


def build_corpus(refgen, seed=1, scale=1.0, game_configs=GAME_CONFIGS):
    rng = random.Random(seed)
    corpus = {}
    positions, games = [], []
    seen = set()
    for tag, gseed, n, plies, sample, bias in game_configs:
        p, g = ref_games(refgen, gseed + seed, max(1, int(n * scale)), plies, sample, bias, tag)
        for r in p:
            key = " ".join(r["sfen"].split()[:3])
            if key not in seen:
                seen.add(key)
                positions.append(r)
        games += g
    # transitions of the sampled game positions (captures, promotions and
    # two-step moves; quiet moves are covered by the game replays)
    trans = ref_moves(refgen, [r["sfen"] for r in positions], "nontrivial")
    for r, t in zip(positions, trans):
        assert not isinstance(t, str) and t["expected"] == r["expected"], r["id"]
        r["next"] = t["next"]
    corpus["games"] = positions + games
    corpus["pieces"] = evaluate(refgen, piece_cases(rng), "nontrivial")
    corpus["scatter"] = evaluate(refgen, scatter_cases(rng, max(1, int(2000 * scale))), "nontrivial")
    corpus["targeted"] = evaluate(refgen, targeted_cases())
    corpus["rule_games"] = rule_game_records(refgen)
    return corpus


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--refgen", required=True)
    ap.add_argument("--out", default="tests/data")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--scale", type=float, default=1.0)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for name, records in build_corpus(a.refgen, a.seed, a.scale).items():
        write(os.path.join(a.out, f"{name}.jsonl.gz"), records)


if __name__ == "__main__":
    main()
