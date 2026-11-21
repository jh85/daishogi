"""AlphaZero input encoding for dai shogi — Python mirror of src/az/encode.cpp.

Ported from ../chushogi (az/encode.py, commit d578c81), adapted to the
15x15 board, two-letter SFEN codes and the 180-degree flip.

Positions are canonicalized to the side-to-move's perspective (180-degree
rotation + color swap when Gote is to move), then encoded as 114 planes of
15x15:

  planes   0..54  : our pieces, one plane per kind (order = C++ Kind enum)
  planes  55..109 : their pieces
  plane  110      : one-hot on the lion-capture square (先獅子 state)
  plane  111      : broadcast 1 if the position occurred once before
  plane  112      : broadcast 1 if it occurred twice or more before
  plane  113      : broadcast ply / PLY_CAP (draw-cap progress)

Policy space (115,650 logits): from x to (225x225 = 50,625), a second
from x to matrix for promotions, and a from x midDir(8) x toDir(8) block
(14,400) for two-step lion-power moves (lion, lion from kirin, horned
falcon, soaring eagle; incl. igui and passes). Moves must be in the
canonical (flipped) frame.
"""

import numpy as np

FILES = RANKS = 15
BOARD = FILES * RANKS
NUM_PLANES = 114
PLY_CAP = 1500

POLICY_MOVE = BOARD * BOARD          # 50,625
POLICY_DOUBLE = BOARD * 8 * 8        # 14,400
POLICY_SIZE = 2 * POLICY_MOVE + POLICY_DOUBLE

# SFEN codes in C++ Kind enum order (src/types.h / src/types.cpp).
KIND_CODES = [
    'PA', 'LA', 'KN', 'SI', 'GO', 'BI', 'RO', 'KI', 'DH', 'DK', 'LI', 'DE',
    'CO', 'FL', 'RC', 'BT', 'KR', 'PH', 'GB', 'SM', 'VM', 'FK', 'IR', 'ST',
    'EW', 'AB', 'CS', 'VO', 'FD',
    '+PA', '+LA', '+KN', '+IR', '+ST', '+EW', '+AB', '+CS', '+VO', '+FD',
    '+SI', '+GO', '+BI', '+RO', '+DH', '+DK', '+DE', '+CO', '+FL', '+RC',
    '+BT', '+KR', '+PH', '+GB', '+SM', '+VM',
]
KIND_INDEX = {c: i for i, c in enumerate(KIND_CODES)}
KINDS = len(KIND_CODES)              # 55
LION_CAPTURE_PLANE = 2 * KINDS       # 110

_DIRS = [(-1, -1), (0, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (0, 1), (1, 1)]
_DIR_INDEX = {d: i for i, d in enumerate(_DIRS)}


def parse_square(s):
    """'8h' -> square index (x = file - 1, y = rank - 'a')."""
    i = 0
    while s[i].isdigit():
        i += 1
    return (int(s[:i]) - 1) + (ord(s[i]) - ord('a')) * FILES


def square_name(sq):
    return f"{sq % FILES + 1}{chr(ord('a') + sq // FILES)}"


def flip_square(sq):
    """180-degree rotation (src/position.h flipSquare)."""
    return BOARD - 1 - sq


def split_squares(usi):
    """'8h8g8f+' -> ([sq, sq, sq], promote)."""
    promote = usi.endswith('+')
    if promote:
        usi = usi[:-1]
    squares = []
    i = 0
    while i < len(usi):
        j = i
        while usi[j].isdigit():
            j += 1
        squares.append(parse_square(usi[i:j + 1]))
        i = j + 1
    return squares, promote


def flip_usi_move(token):
    squares, promote = split_squares(token)
    return ''.join(square_name(flip_square(s)) for s in squares) + ('+' if promote else '')


class Position:
    """Board state parsed from SFEN (no move generation here)."""

    def __init__(self, sfen):
        parts = sfen.split()
        board, self.side, lion, num = parts[0], parts[1], parts[2], parts[3]
        self.board = [None] * BOARD  # sq -> (kind index, color 0=sente 1=gote)
        ranks = board.split('/')
        if len(ranks) != RANKS:
            raise ValueError(f"expected {RANKS} ranks: {sfen}")
        for y, rank in enumerate(ranks):
            x = FILES - 1  # each rank lists files 15 -> 1
            i = 0
            while i < len(rank):
                c = rank[i]
                if c.isdigit():
                    j = i
                    while j < len(rank) and rank[j].isdigit():
                        j += 1
                    x -= int(rank[i:j])
                    i = j
                    continue
                promoted = c == '+'
                if promoted:
                    i += 1
                code = rank[i:i + 2]
                if len(code) != 2 or not code.isalpha() or not (code.isupper() or code.islower()):
                    raise ValueError(f"bad piece code {code!r}: {sfen}")
                kind = KIND_INDEX[('+' if promoted else '') + code.upper()]
                color = 0 if code.isupper() else 1
                self.board[y * FILES + x] = (kind, color)
                x -= 1
                i += 2
            if x != -1:
                raise ValueError(f"rank {y} does not have {FILES} squares: {sfen}")
        self.side_to_move = 0 if self.side == 'b' else 1
        self.lion_capture = None if lion == '-' else parse_square(lion)
        self.move_number = int(num)

    def flipped(self):
        out = Position.__new__(Position)
        out.board = [None] * BOARD
        for s, pc in enumerate(self.board):
            if pc:
                out.board[flip_square(s)] = (pc[0], pc[1] ^ 1)
        out.side = 'w' if self.side == 'b' else 'b'
        out.side_to_move = self.side_to_move ^ 1
        out.lion_capture = (None if self.lion_capture is None
                            else flip_square(self.lion_capture))
        out.move_number = self.move_number
        return out


def move_to_index(usi):
    """USI move (canonical frame) -> policy index."""
    squares, promote = split_squares(usi)
    if len(squares) == 2:
        idx = squares[0] * BOARD + squares[1]
        return POLICY_MOVE + idx if promote else idx
    f, mid, to = squares
    fx, fy = f % FILES, f // FILES
    mx, my = mid % FILES, mid // FILES
    tx, ty = to % FILES, to // FILES
    md = _DIR_INDEX[(mx - fx, my - fy)]
    td = _DIR_INDEX[(tx - mx, ty - my)]
    return 2 * POLICY_MOVE + f * 64 + md * 8 + td


def index_to_move(idx):
    """Policy index -> USI move (canonical frame)."""
    if idx < 2 * POLICY_MOVE:
        promote = idx >= POLICY_MOVE
        i = idx % POLICY_MOVE
        return square_name(i // BOARD) + square_name(i % BOARD) + ('+' if promote else '')
    i = idx - 2 * POLICY_MOVE
    f, md, td = i // 64, (i // 8) % 8, i % 8
    fx, fy = f % FILES, f // FILES
    mx, my = fx + _DIRS[md][0], fy + _DIRS[md][1]
    tx, ty = mx + _DIRS[td][0], my + _DIRS[td][1]
    return square_name(f) + square_name(my * FILES + mx) + square_name(ty * FILES + tx)


def encode_position(pos, rep_count=0, ply=None):
    """Position -> float32 array of shape (NUM_PLANES, 15, 15)."""
    if ply is None:  # moveNumber counts plies + 1
        ply = pos.move_number - 1
    if pos.side_to_move == 1:
        pos = pos.flipped()
    out = np.zeros((NUM_PLANES, BOARD), dtype=np.float32)
    for s, pc in enumerate(pos.board):
        if pc:
            kind, color = pc
            plane = kind if color == pos.side_to_move else KINDS + kind
            out[plane, s] = 1.0
    if pos.lion_capture is not None:
        out[LION_CAPTURE_PLANE, pos.lion_capture] = 1.0
    if rep_count >= 1:
        out[LION_CAPTURE_PLANE + 1, :] = 1.0
    if rep_count >= 2:
        out[LION_CAPTURE_PLANE + 2, :] = 1.0
    out[LION_CAPTURE_PLANE + 3, :] = min(ply / PLY_CAP, 1.0)
    return out.reshape(NUM_PLANES, RANKS, FILES)
