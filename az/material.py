"""Material scoring of capped self-play games.

A game that reaches the ply cap is a draw under the rules and stays one in
matches. Only its value training target changes. The side ahead in material
at the cap gets a partial win:

    d = (material(Sente) - material(Gote)) / material of one side at the start
    t = max_score * tanh(d / scale)            (Sente's view; Gote's is -t)
    WDL target = [t, 1 - t, 0] if t >= 0 else [0, 1 + t, -t]

so a capped game is never worth more than `max_score` of a win, and a real
win (royal capture) is always worth more.

Piece values are HaChu's (H.G. Muller's engine for chu, dai and other large
shogi variants; hachu.c 0.21, Debian hachu 0.21-29-gdf26f4a-4): the eight
dai-only pieces from `daiPieces[]`, the rest from `chuPieces[]`, which the
dai variant falls back to. A promoted piece takes the value of the piece it
becomes (e.g. a promoted pawn is a gold). Royals keep HaChu's values: kings
cancel out while both sides have one, and a crown prince (a promoted drunk
elephant) counts as the extra royal it is.
"""

import math

import encode as E

# Unpromoted kinds, by SFEN code (HaChu name in the comment).
_BASE = {
    'PA': 40,    # P
    'LA': 150,   # L
    'KN': 60,    # N (dai knight; daiPieces)
    'SI': 100,   # S
    'GO': 151,   # G
    'BI': 250,   # B
    'RO': 300,   # R
    'KI': 280,   # K
    'DH': 350,   # DH
    'DK': 400,   # DK
    'LI': 1000,  # LN (LVAL)
    'DE': 201,   # DE
    'CO': 100,   # C
    'FL': 150,   # FL
    'RC': 150,   # RV
    'BT': 152,   # BT
    'KR': 154,   # KN (kirin)
    'PH': 153,   # PH
    'GB': 50,    # GB
    'SM': 200,   # SM
    'VM': 200,   # VM
    'FK': 600,   # FK
    'IR': 80,    # I  (daiPieces)
    'ST': 50,    # SG (daiPieces)
    'EW': 80,    # EW (daiPieces)
    'AB': 60,    # AB (daiPieces)
    'CS': 70,    # CS (daiPieces)
    'VO': 200,   # VO (daiPieces)
    'FD': 150,   # FD (daiPieces)
}
# Promoted kinds: the value of the piece they move as.
_PROMOTED = {
    '+PA': 151, '+KN': 151, '+IR': 151, '+ST': 151, '+EW': 151, '+AB': 151,
    '+CS': 151, '+VO': 151, '+FD': 151,  # gold
    '+LA': 350,   # white horse
    '+SI': 200,   # vertical mover
    '+GO': 300,   # rook
    '+BI': 350,   # dragon horse
    '+RO': 400,   # dragon king
    '+DH': 500,   # horned falcon
    '+DK': 550,   # soaring eagle
    '+DE': 270,   # crown prince
    '+CO': 200,   # side mover
    '+FL': 250,   # bishop
    '+RC': 250,   # whale
    '+BT': 300,   # flying stag
    '+KR': 1000,  # lion
    '+PH': 600,   # free king
    '+GB': 201,   # drunk elephant
    '+SM': 400,   # free boar
    '+VM': 400,   # flying ox
}
VALUES = [{**_BASE, **_PROMOTED}[c] for c in E.KIND_CODES]  # by kind index

DEFAULT_MAX_SCORE = 0.5
DEFAULT_SCALE = 0.1


def material(sfen):
    """-> (Sente's material, Gote's material) on the board."""
    pos = E.Position(sfen)
    side = [0, 0]
    for pc in pos.board:
        if pc:
            side[pc[1]] += VALUES[pc[0]]
    return side[0], side[1]


def cap_score(initial_sfen, final_sfen, max_score=DEFAULT_MAX_SCORE,
              scale=DEFAULT_SCALE):
    """Signed partial-win score t in [-max_score, max_score], Sente's view."""
    army = material(initial_sfen)[0]
    s, g = material(final_sfen)
    return max_score * math.tanh((s - g) / army / scale)


def wdl(t):
    """Score t (side to move's view) -> [win, draw, loss] target."""
    return [t, 1.0 - t, 0.0] if t >= 0 else [0.0, 1.0 + t, -t]
