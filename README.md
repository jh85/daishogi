# Dai shogi (大将棋) in C++

A C++17 implementation of dai shogi (15×15, 130 pieces, 29 kinds). It
provides:

* SFEN-style positions with two-letter piece codes, and USI-style moves
  (parse and write);
* a complete legal move generator, checked against Shogitter's own engine
  ([haitaka-variants](https://github.com/na2hiro/haitaka-variants), rule
  49) under its promotion rule (`--promotion normal`);
* game-end detection: royal capture, no legal move, fourfold repetition,
  perpetual check;
* random-versus-random play with reproducible seeds.

```
src/            engine (no dependencies beyond the C++17 standard library)
  types.*         piece table, coordinates, lion-power continuations
  position.*      board, move application, attack tests, hashing
  notation.*      SFEN / USI parsing and output, diagrams
  movegen.*       legal move generator, perft
  game.*          game history and termination rules
  mate.*          checkmate solver (BNS / df-pn, ported from ../chushogi)
  main.cpp        command-line tool `daishogi`
  az/             AlphaZero search, self-play worker `daishogi-az` (see az/README.md)
az/             AlphaZero network, training loop, GPU eval server (Python)
tests/          unit tests, reference comparison harness, committed corpus
  az/             AlphaZero tests (`make az-test`)
tools/refgen/   small Rust driver for ../haitaka-variants (reference data)
docs/FORMAT.md  position and move formats
docs/RULES.md   rules as implemented, sources, ambiguities, limitations
docs/SFEN-DESIGN.md  design and rationale of the two-letter piece codes
```

## Build

```sh
make                # builds ./daishogi (needs g++ or clang++ with C++17)
make daishogi-az    # AlphaZero self-play worker (see az/README.md)
make az-test AZPY=~/venv/bin/python   # AlphaZero tests (NumPy + PyTorch)
make mate-test      # mate solver vs brute force (~2 min)
make COUNTER_STRIKE=jcsa   # another default 先獅子 variant (all | lishogi | jcsa); `make clean` first
make PROMOTION=normal      # haitaka-variants' promotion rule as the default (chu | normal); `make clean` first
```

The 先獅子 rule follows haitaka-variants by default. The JCSA and lishogi
variants protect fewer lions; select one at runtime with
`daishogi --counter-strike jcsa ...` (also `daishogi-az`). See
docs/RULES.md, "Counter-strike (先獅子) variants".

Promotion follows chu shogi by default. `--promotion normal` (also for
`daishogi-az`) selects haitaka-variants' rule instead; the comparison tests
against haitaka-variants use it. See docs/RULES.md, "Promotion".

`tools/refgen` is needed only to regenerate reference data. It needs Rust
(`cargo`) and the `../haitaka-variants` checkout, which it uses unmodified:

```sh
make tools/refgen/target/release/refgen
```

## Run the tests

```sh
make test           # unit tests + full comparison with the committed reference corpus
```

`make test` runs three steps. None of them needs Rust.

1. `tests/unit_tests`: 815 checks covering notation, the initial setup,
   perft, rule examples, the 先獅子 variants and the side-to-move flip.
2. `tests/compare.py`: 14,251 reference positions with 1,716,277 reference
   moves (adjusted for the two deliberate differences above). For each
   position the complete legal-move sets must be identical.
   The position's result must match, and so must the position reached by
   each of 373,115 recorded moves: all moves for hand-written cases;
   captures, promotions and two-step moves for the rest.
3. `tests/replay_games.py`: 134 reference games (75,231 plies) replayed
   move by move. The results and final positions must match. This includes
   repetition draws, perpetual check, prince-after-king, no-legal-move and
   先獅子 games.

Any mismatch is printed and written to `tests/mismatches.jsonl` with the
SFEN, the missing moves and the extra moves. Reproduce one with
`./daishogi moves "<sfen>"`.

With Rust available, more checks run straight against the reference:

```sh
make reftest SEED=7 GAMES=100   # fresh corpus with a new seed, plus our own random
                                # games replayed by the reference, plus perft 1-3
make corpus                     # regenerate tests/data (byte-reproducible)
python3 tests/mutation_check.py # break 19 rules one at a time; all must be caught
```

The committed corpus (`tests/data/*.jsonl.gz`, about 6 MB) contains:

| File | Contents |
|---|---|
| `games` | positions sampled from 120 reference-played games (uniform random, and random biased towards captures), plus the game records |
| `pieces` | every piece kind of both colors on 9 squares, alone and with random surroundings |
| `scatter` | 2,000 random placements, dense around lions and royals, with random 先獅子 state and side to move |
| `targeted` | 66 hand-written rule cases (lion protection, x-ray protection, 付け喰い, 先獅子, falcon/eagle, royal capture, promotion, dead ranks, file limits, range-2 pieces), each also mirrored |
| `rule_games` | 14 scripted games: repetition, perpetual check, lion passes, prince, no legal move, 先獅子 after falcon moves |

Results at the time of writing:
* all positions, transitions and games match (with 154 `ours_only` and 4
  `reference_only` moves);
* perft from the start position matches the reference: 71, 5,041, 358,191
  and 25,451,513 (depth 4);
* a fresh `make reftest SEED=4242 GAMES=60` passes, including 75 of our own
  random games (52k plies) replayed by the reference;
* all 19 mutants are detected.

## Play

```sh
./daishogi play --seed 42                         # one random-vs-random game
./daishogi play --seed 1 --games 100 --quiet      # 100 games, summary only
./daishogi play --seed 7 --max-plies 300 --show   # stop after 300 plies, print the board
make play SEED=42 GAMES=3                          # the same through make
```

Both players pick uniformly from the legal moves. Each uses its own
splitmix64 stream derived from the game seed, so a seed reproduces a game
exactly on any platform. Game *i* of a run uses seed `seed + i - 1`. For
each game the tool prints the result, the plies played, the start SFEN, all
moves and the final SFEN. A game stopped by `--max-plies` is reported as
`cutoff (move limit reached; test cutoff, not a game result)`.

Random games usually end with a royal capture after 300–2,000 plies. About
50 games run per second.

## Other commands

```sh
./daishogi show startpos                      # diagram, SFEN, status
./daishogi moves startpos 8k8j 8e8f           # legal moves after some moves
./daishogi moves "14ki/15/15/15/15/7go7/7li7/7LI7/15/15/15/15/15/15/KI14 b - 1"
./daishogi perft 3 startpos --divide
./daishogi batch < commands.txt               # line protocol, see docs/FORMAT.md
```

## Rules in brief

The rules follow Shogitter rule 49; [docs/RULES.md](docs/RULES.md) has all
details and the reasons for each choice.

* No drops, and no check rule.
* A player who captures all of the opponent's royal pieces (king and
  prince) wins.
* The side to move with no legal move loses.
* The fourth occurrence of a position is a draw, unless one side gave
  perpetual check; then that side loses.
* Promotion (chu rule, the default) is allowed on a move that enters the
  far five ranks, on a move that starts there and captures, and on a move
  to a rank where the piece could never move again (the last rank, or the
  last two for a knight). It is never compulsory: a piece that does not
  promote there stays as a dead piece. `--promotion normal` selects
  haitaka-variants' rule instead: any move that starts or ends in the zone,
  compulsory where the piece could not move again.
* The chu shogi lion capture rules apply: adjacent lions, protected lions,
  付け喰い, and 先獅子. A lion may always capture an adjacent lion.
* No more than one unpromoted pawn, and one unpromoted go-between, per side
  per file.

Apart from promotion, where the reference's behaviour could be read
differently from the written rules, this implementation follows the
reference. These cases are
falcon/eagle two-step moves under 先獅子, two-step lion captures, and igui
on the last royal piece. Two exceptions follow lishogi and the JCSA
instead, both for lion two-step moves that take a lion:
* a lion adjacent to the start square may always be taken, also on the
  second step;
* a pawn or go-between taken on the first step no longer blocks a protector
  of the lion taken on the second step.

The test corpus lists the reference moves these rules forbid as
`reference_only` and the moves they add as `ours_only`. [docs/RULES.md](docs/RULES.md#discrepancies-and-choices)
lists them with test cases.
