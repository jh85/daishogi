"""Conversion between the haitaka-variants wire format (Shogitter rule 49)
and our SFEN / USI notation (docs/FORMAT.md).

Kept independent of the C++ piece table on purpose: this is the harness's
own definition of the common format, so a wrong entry in the engine's table
cannot hide itself.

Reference wire coordinates are [x, y], both 1-based: x is the file number
(1 = Sente's right edge) and y the rank counted from Gote's back rank. Our
square name is "<x><letter>" with letter = "abcdefghijklmno"[y-1], so the
two coordinate systems agree square for square.
"""

FILES = RANKS = 15

# our piece code (Sente / uppercase) -> (reference id, reference origin)
CODE_TO_REF = {
    "PA": ("aa", None), "LA": ("ab", None), "KN": ("ac", None), "SI": ("ad", None),
    "GO": ("ae", None), "BI": ("af", None), "RO": ("ag", None), "KI": ("ah", None),
    "DH": ("am", None), "DK": ("an", None), "LI": ("bl", None), "DE": ("cf", None),
    "CO": ("ci", None), "FL": ("cl", None), "RC": ("cm", None), "BT": ("cn", None),
    "KR": ("co", None), "PH": ("cp", None), "GB": ("cq", None), "SM": ("cr", None),
    "VM": ("cs", None), "FK": ("ct", None), "IR": ("dp", None), "ST": ("dq", None),
    "EW": ("dr", None), "AB": ("ds", None), "CS": ("dt", None), "VO": ("du", None),
    "FD": ("dv", None),
    "+PA": ("ai", None), "+LA": ("cu", None),
    "+KN": ("do", "ac"), "+IR": ("do", "dp"), "+ST": ("do", "dq"),
    "+EW": ("do", "dr"), "+AB": ("do", "ds"), "+CS": ("do", "dt"),
    "+VO": ("do", "du"), "+FD": ("do", "dv"),
    "+SI": ("de", None), "+GO": ("dd", None), "+BI": ("dl", None), "+RO": ("dm", None),
    "+DH": ("cz", None), "+DK": ("db", None), "+DE": ("cg", None), "+CO": ("df", None),
    "+FL": ("dg", None), "+RC": ("cv", None), "+BT": ("cw", None), "+KR": ("dh", None),
    "+PH": ("dj", None), "+GB": ("dk", None), "+SM": ("cx", None), "+VM": ("cy", None),
}
REF_TO_CODE = {v: k for k, v in CODE_TO_REF.items()}
assert len(CODE_TO_REF) == 55 and len(REF_TO_CODE) == 55

# Unpromoted codes of the initial setup and the per-side counts there
# (the reference rejects positions exceeding them, counting a promoted
# piece under its unpromoted origin).
INITIAL_COUNT = {
    "PA": 15, "LA": 2, "KN": 2, "SI": 2, "GO": 2, "BI": 2, "RO": 2, "KI": 1, "DH": 2,
    "DK": 2, "LI": 1, "DE": 1, "CO": 2, "FL": 2, "RC": 2, "BT": 2, "KR": 1, "PH": 1,
    "GB": 2, "SM": 2, "VM": 2, "FK": 1, "IR": 2, "ST": 2, "EW": 2, "AB": 2,
    "CS": 2, "VO": 2, "FD": 2,
}
PROMOTABLE = [c for c in INITIAL_COUNT if "+" + c in CODE_TO_REF]
# Ranks (counted from the far end) an unpromoted piece may not stand on.
DEAD_RANKS = {"PA": 1, "LA": 1, "IR": 1, "ST": 1, "KN": 2}
ROYAL = {"KI", "+DE"}


def base_code(code):
    """Unpromoted origin of a code ('+KN' -> 'KN')."""
    return code[1:] if code.startswith("+") else code


def square_name(x, y):
    return f"{x}{'abcdefghijklmno'[y - 1]}"


def parse_square(s):
    return int(s[:-1]), "abcdefghijklmno".index(s[-1]) + 1


# ---------------------------------------------------------------- positions

def parse_sfen(sfen):
    """SFEN -> (pieces {(x, y): (code, color)}, side 0/1, lion square|None)."""
    fields = sfen.split()
    rows = fields[0].split("/")
    if len(rows) != RANKS:
        raise ValueError("expected 15 ranks")
    pieces = {}
    for y, row in enumerate(rows, start=1):
        x = FILES
        i = 0
        while i < len(row):
            c = row[i]
            if c.isdigit():
                j = i
                while j < len(row) and row[j].isdigit():
                    j += 1
                x -= int(row[i:j])
                i = j
                continue
            code = ""
            if c == "+":
                code, i = "+", i + 1
            letters = row[i:i + 2]
            if len(letters) != 2 or not letters.isalpha() or not (
                    letters.isupper() or letters.islower()):
                raise ValueError(f"bad piece code {letters!r}")
            code += letters.upper()
            i += 2
            if code not in CODE_TO_REF:
                raise ValueError(f"unknown code {code}")
            pieces[(x, y)] = (code, 0 if letters.isupper() else 1)
            x -= 1
        if x != 0:
            raise ValueError(f"rank {y} has wrong length")
    side = 0 if len(fields) < 2 or fields[1] == "b" else 1
    lion = None
    if len(fields) > 2 and fields[2] != "-":
        lion = parse_square(fields[2])
    return pieces, side, lion


def make_sfen(pieces, side, lion=None, move_number=1):
    rows = []
    for y in range(1, RANKS + 1):
        row, run = "", 0
        for x in range(FILES, 0, -1):
            if (x, y) not in pieces:
                run += 1
                continue
            if run:
                row += str(run)
                run = 0
            code, color = pieces[(x, y)]
            row += code if color == 0 else code.lower()
        if run:
            row += str(run)
        rows.append(row)
    lion_s = square_name(*lion) if lion else "-"
    return f"{'/'.join(rows)} {'b' if side == 0 else 'w'} {lion_s} {move_number}"


def wire_to_sfen(wire, last_move=None):
    """Reference wire position -> SFEN. The reference stores only whether the
    lion-capture restriction is active; our third field names the square,
    which is the destination of the move that set it (last_move)."""
    pieces = {}
    for p in wire["pieces"]:
        code = REF_TO_CODE[(p["piece"], p.get("origin"))]
        pieces[tuple(p["at"])] = (code, p["color"])
    lion = None
    if wire.get("state", {}).get("senjishi"):
        if last_move is None:
            raise ValueError("senjishi set but the capturing move is unknown")
        lion = tuple(last_move["path"][-1])
    return make_sfen(pieces, wire["sideToMove"], lion)


def sfen_to_wire(sfen):
    pieces, side, lion = parse_sfen(sfen)
    out = []
    for (x, y), (code, color) in sorted(pieces.items()):
        ref, origin = CODE_TO_REF[code]
        p = {"at": [x, y], "piece": ref, "color": color}
        if origin:
            p["origin"] = origin
        out.append(p)
    return {"sideToMove": side, "pieces": out, "hands": [{}, {}],
            "state": {"senjishi": lion is not None}}


def state_key(sfen):
    """Board, side and lion-capture flag (what the rules depend on)."""
    f = sfen.split()
    return (f[0], f[1], f[2] != "-")


def state_hash(sfen):
    """Short digest of state_key, used to store expected transitions."""
    import hashlib
    return hashlib.sha1(" ".join(map(str, state_key(sfen))).encode()).hexdigest()[:8]


def wire_state_hash(wire, move):
    """state_hash of a reference position reached by `move` (reference JSON)."""
    return state_hash(wire_to_sfen(wire, move))


def open_jsonl(path):
    import gzip
    return gzip.open(path, "rt") if path.endswith(".gz") else open(path)


# -------------------------------------------------------------------- moves

def usi_to_ref_move(usi):
    import re
    promotion = usi.endswith("+")
    squares = [parse_square(m) for m in re.findall(r"\d+[a-o]", usi)]
    return {"kind": "move", "schemaVersion": 3, "from": list(squares[0]),
            "path": [list(q) for q in squares[1:]], "promotion": promotion}


def ref_move_to_usi(mv):
    s = square_name(*mv["from"]) + "".join(square_name(*sq) for sq in mv["path"])
    return s + ("+" if mv["promotion"] else "")


def ref_outcome_token(outcome, cutoff=False):
    if cutoff or outcome["kind"] == "ongoing":
        return "ongoing"
    if outcome["kind"] == "draw":
        return "draw"
    return "win " + ("sente" if outcome["winner"] == 0 else "gote")


def engine_outcome_token(status_line):
    """'status win sente royals' -> 'win sente'; 'status draw repetition' -> 'draw'."""
    t = status_line.split()[1:]
    if t[0] == "win":
        return "win " + t[1]
    return t[0]


# ------------------------------------------------------------------ engine

# The reference (haitaka-variants) uses the Normal promotion rule; our
# default is the chu rule (docs/RULES.md), so every comparison pins it.
REF_RULES = ["--promotion", "normal"]


class Engine:
    """Our binary in batch mode."""

    def __init__(self, binary):
        import subprocess
        self.p = subprocess.Popen([binary, *REF_RULES, "batch"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)

    def ask(self, *lines):
        for line in lines[:-1]:
            self.p.stdin.write(line + "\n")
        self.p.stdin.write(lines[-1] + "\n")
        self.p.stdin.flush()
        return self.p.stdout.readline().rstrip("\n")

    def _position_then(self, cmd, query):
        # "position" prints nothing on success and one error line on failure;
        # the query always prints one line.
        r = self.ask(cmd, query)
        if r.startswith("error"):
            self.p.stdout.readline()  # the query's answer for the old game
            raise RuntimeError(f"{r} ({cmd[:200]})")
        return r

    def set_position(self, sfen, moves=()):
        cmd = "position sfen " + sfen
        if moves:
            cmd += " moves " + " ".join(moves)
        return self._position_then(cmd, "sfen")[len("sfen "):]

    def status(self):
        return self.ask("status")

    def legal(self, sfen):
        return self._position_then("position sfen " + sfen, "legal").split()[1:]

    def transitions(self, sfen):
        """{usi: sfen after the move} for every legal move."""
        first = self._position_then("position sfen " + sfen, "transitions")
        out = {}
        line = first
        while line != "end":
            usi, rest = line.split(" ", 1)
            out[usi] = rest
            line = self.p.stdout.readline().rstrip("\n")
        return out

    def close(self):
        self.p.stdin.write("quit\n")
        self.p.stdin.close()
        self.p.wait()
