# AlphaZero self-play agent for dai shogi

A port of the AlphaZero pipeline from `../chushogi` (commit `d578c81`) to
dai shogi. The search, self-play loop and training are the chushogi code,
resized for the 15×15 board and wired to this repository's engine. Each
file names its chushogi origin in its header.

Status: **ported, tested and benchmarked; not yet trained.** The network
and search settings below are measured starting points, not tuned ones
(see "Open points").

## Pieces

| Part | Files | Notes |
|---|---|---|
| Encoding | `src/az/encode.*`, `az/encode.py` | 114 planes of 15×15; 115,650 policy logits |
| Search | `src/az/mcts.*` | PUCT, FPU, virtual loss, batched leaf eval, root Dirichlet noise |
| Mate solver | `src/mate.*` | chushogi's BNS / df-pn checkmate solver: the root mate override and in-tree probes |
| Self-play | `src/az/selfplay.*` | one game per call, JSON records with packed planes |
| Evaluator client | `src/az/eval_client.*` | TCP to the GPU server, fp16 on the wire |
| Command-line worker | `src/az/main.cpp` → `daishogi-az` | self-play, `go az` match play, encoding inspection |
| Network | `az/model.py` | transformer over the 225 squares; WDL value head |
| GPU server | `az/eval_server.py`, `az/inference_batcher.py` | reloads the checkpoint when it changes (`--watch`) |
| Training | `az/train.py`, `az/buffer.py`, `az/optimizers.py` | soft cross-entropy on visits + WDL; value target z / soft-Z / A0GB / blend |
| Loop | `az/run_loop.py`, `az/selfplay_pool.py` | W workers × G games per iteration, then train and publish |
| Strength | `az/eval_match.py`, `az/checkpoint_tournament.py` | vs the random player or between checkpoints |
| Benchmark | `az/bench_selfplay.py` | evaluations/s and plies/s of W workers on one server |
| Monitoring | `az/run_status.py`, `az/heldout_loss.py` | per-iteration game summary; loss on unseen games vs baselines |
| Tests | `tests/az/` | `make az-test`, see below |

### Encoding

The side to move always sees the board as Sente. When Gote is to move, the
position is flipped: rotated 180° with the colours swapped
(`Position::flipped()`). Dai shogi's initial setup is invariant under this
flip, so both sides see the same start position.

* **Input planes (114):** 55 for our pieces (one per kind, `src/types.h`
  order), 55 for theirs, the lion-capture square (先獅子), two repetition
  planes (occurred once / twice or more before), and game progress
  (ply / 1500).
* **Policy (115,650):** a from × to grid (225 × 225), a second one for
  promotions, and 225 × 8 × 8 entries for the two-step moves of the lion,
  the lion from kirin, the horned falcon and the soaring eagle, indexed by
  the directions of the two steps. That includes igui and passes.

## Differences from the chushogi code

| Change | Why |
|---|---|
| Flip = 180° rotation (chushogi mirrors the ranks only) | Our Gote moves are defined as the point reflection of Sente's, and dai shogi's start is symmetric under it. A rank mirror would put the kirin and phoenix on the wrong files. |
| Repetition keys include the real side to move | Chushogi keys positions on their flipped view, so a position and its flipped twin share a key. In dai shogi that happens on the first lion pass from the start (the board is unchanged, Gote to move), which the search would score as a repetition draw. `tests/az/search_value_test` fails on chushogi's keying. |
| Games are adjudicated by `dai::Game` | Fourfold repetition, perpetual check (the checker loses), royal capture, no legal move. Chushogi's self-play treats every fourfold repetition as a draw. |
| Policy indices are 32-bit on the wire | 115,650 move slots do not fit 16 bits. |
| Packed planes padded to whole bytes (3,179 per position) | 113 × 225 bits is not a multiple of 8. |
| Game cap 1,500 plies (chushogi: 1,000) | Dai shogi games are longer; random games average about 800 plies. |
| GPU server: bf16 weights, no autocast, bit planes unpacked on the GPU | The forward pass is memory-bound on the GB10. Autocast re-casts the weights and runs the norms in fp32; together these changes took self-play from 3,500 to 6,300 evaluations/s. Logits stay within 0.1 of autocast. |
| Training uses bf16 autocast (`--amp bf16`, the default; `--amp off` = chushogi) | Twice as fast (1,217 vs 616 positions/s for 256 × 6). The weights and optimizer state stay fp32. |
| The network size travels in the checkpoint | The server and resumed training rebuild the net from it. `run_loop.py --d-model/--blocks/--heads` sets it for a new run. |
| `selfplay --start position` | Starts games from the position set by `position` (tests, opening books). |
| Positions where the side to move can take the opponent's last royal are proven wins in the tree; the root plays such a capture at once (one-hot target) | The search otherwise misses the opponent's king capture one ply deeper (see "Training runs", run2). An exact rule fact; it replaces part of what chushogi's mate probes did. |
| Mate solver: check filtering by lines, a TT generation counter, two-royal defenders out of scope at every node | About 3.7× faster per node than a direct port (0.28 ms per depth-3 probe on mate-rich positions). Chushogi only gave up on a two-royal defender with no evasion left; skipping the sacrifice lines is not sound. |
| Mate lines must end before the game cap; a root mate PV must not reach a fourth occurrence | The solver models neither rule. Chushogi's self-play checked only the cap, and only for proven losses. |
| Proven-losing root moves get no policy-target share and are never chosen (unless every move loses) | A strong wrong prior keeps sending visits to a move already known to lose; visit sampling would still play it. |
| Playout cap randomization (`--full-prob`, `--fast-sims`; off by default) | KataGo's scheme: only a random share of moves get the full search and are recorded. See "Training runs" for why. |
| `train.py --sample-fraction` (off by default) | Trains on a stable-hashed share of each game's positions. |

**Left out for now:**
* **Proof-aware root move selection** and the USI-mode repetition guard
  for mate lines.
* **The experimental safe-sampling and safe-policy options.**
* **The USI engine mode** for playing in a GUI.
* **The experiment, audit and benchmark scripts.** `bench_selfplay.py`
  is new.

**Search approximation (same as chushogi):** inside the search tree, a
repeated position is scored as a draw on its first recurrence. Perpetual
check is not told apart from repetition there. The real game result is
still exact.

## Build, test and run

```sh
make daishogi                 # the rules engine (random player, referee)
make daishogi-az              # the self-play worker / match player
make az-test AZPY=~/venv/bin/python   # needs NumPy and PyTorch; ~4 min
make mate-test                # the mate solver alone (~2 min)
```

`make az-test` runs these tests. The chushogi tests for the parts that were
left out (USI play, experiment scripts) are not ported.

| Test | Ported from chushogi | Checks |
|---|---|---|
| `../mate_test.cpp` | `mate_test.py` | there is no dai shogi tsume collection, so the solver is checked against brute force instead. The positions are 1,355 from the last plies of self-play games lost by royal capture (`tests/data/mate_positions.txt`): the verdict equals exhaustive search of the same model at depths 3, 5 and 9 (237 mates at depth 9, PVs up to 7 plies), for BNS and df-pn. Also checked: PVs replay legally, full-rule soundness at depth 3, flip symmetry, and that the check and evasion filters equal `royalAttacked` on 578,797 moves |
| `search_value_test.cpp` | `az_search_value_test` | value perspective, A0GB, canonical leaf inputs, batching, fourfold history, repetition planes, root temperature; new: real-frame repetition keys with a flipped root and the flipped-twin case |
| `selection_test.cpp` | `az_sampling_test`, parts of `az_root_safety_test` | visit sampling and cutoff; hanging the last royal (prince, lion double capture); root royal filter; win in one; root mate (one-hot policy, ply cap, fourth occurrence); in-tree probes (every reply of a mated defender scores 0) |
| `selfplay_game_test.cpp` | new | a scripted game ends by fourfold repetition at ply 12; each sample's repetition planes, side to move, z |
| `encode_test.py` | `az_encode_test` | C++ = Python planes on 1,088 corpus positions and along the rule games; legal-move indices both ways |
| `selfplay_test.py` | `az_selfplay_test` | records replay in `./daishogi` to the same ending; policy = legal moves (or one-hot on the played move for a proven win); z; planes; also with `--full-prob 0.25` and `--mate` |
| `mcts_test.py` | `az_mcts_test` | the search takes the king in a mate in one, for both sides |
| `pipe_test.py` | `az_pipe_test` | games through the GPU server; the legal-only and full wire formats play the same game |
| `train_test.py` | `az_train_test` | training, exact optimizer resume, the checkpoint drives a game |
| `*_test.py` (unittest) | buffer, legal-policy, eval-output, batcher, optimizers, pool, tournament | 32-bit indices above 65,535, fp16 wire values, batching, optimizer migration, job seeds |

Running:

```sh
# self-play without a network (uniform policy), to check the worker
printf 'selfplay --games 1 --sims 32 --out /path/to/games\nquit\n' | ./daishogi-az

# throughput of a configuration
python az/bench_selfplay.py --workers 16 --sims 128 --batch 16

# a training run (the configuration of run3, see "Training runs")
OMP_NUM_THREADS=1 python az/run_loop.py --run-dir az/data/run3 \
  --workers 16 --games 4 --dynamic-games --sims 128 --batch 16 \
  --full-prob 0.25 --fast-sims 32 --temp-plies -1 --root-policy-temp 1.25 \
  --mate-nodes 5000 --probe-depth 3 --probe-nodes 2000 \
  --eval-max-batch 256 --eval-tcp-nodelay \
  --train-steps 60 --train-batch 256 --max-positions 1000000 \
  --value-target z --lr 1e-3 --lr-decay-steps 100000 --iterations 1000 \
  --strength-every 10 --strength-pairs 10 --strength-sims 128 \
  --stop-file az/data/run3/STOP        # touch it to stop at an iteration boundary

# progress: per-iteration game lengths, endings, results and losses
python az/run_status.py az/data/run3

# value / policy loss on games the net has not trained on (e.g. the
# iteration being played now), against trivial baselines
python az/heldout_loss.py az/data/run3/ckpt/current.pt az/data/run3/games/iter_000012

# strength: an AZ checkpoint against the random player
python az/eval_server.py --port 51589 --checkpoint az/data/run1/ckpt/current.pt &
python az/eval_match.py --port 51589 --games 20 --sims 128
```

`run_loop.py` takes chushogi's other options too. `--eval-servers N`
shards the workers across several GPU servers, `--dynamic-games` hands
games to free workers, and `--strength-every N` plays checkpoint matches.

## Throughput on the GB10

These were measured with `az/bench_selfplay.py`, using a random network of
each size, one eval server with request batching (`--max-batch 256`), and
untrained-net games.

| Network (d × blocks) | Params | Simulations | MCTS batch | Evaluations/s | Plies/s |
|---|---|---|---|---|---|
| 128 × 4 | 1.4 M | 64 | 32 | 14,700 | 229 |
| **256 × 6** | **5.4 M** | 64 | 16 | 5,600 | 88 |
| 256 × 6 | 5.4 M | 64 | 32 | 6,300 | 99 |
| **256 × 6** | **5.4 M** | **128** | **16** | **5,700** | **45** |
| 256 × 6 | 5.4 M | 200 | 32 | 6,500 | 33 |
| 256 × 6 | 5.4 M | 256 | 32 | 6,600 | 26 |
| 384 × 8 | 14.9 M | 64 | 32 | 3,200 | 50 |

* **The GPU is the bottleneck.** One worker searches about 20,000
  simulations/s on its own with a uniform policy, so during self-play the
  workers are nearly idle. The raw network peaks near 7,600 positions/s
  (256 × 6, batch 160). It drops to about 4,700 when 16 cores are busy,
  because the CPU and GPU share memory on the GB10. So extra workers beyond
  what fills the GPU batch do not help.
* **Training** (bf16 autocast, batch 256) runs 1,217 positions/s for
  256 × 6 and 614 for 384 × 8. It shares the GPU with self-play: in the
  loop, the 200 steps of an iteration take about 45 s.

## Training runs

### run1 (stopped after 3 iterations): the value head memorized games

run1 started with the first configuration from step 3: every move searched
with 128 simulations and recorded, 32 games then 200 training steps per
iteration, and sampling for the first 20 plies. It showed two problems.

* **Repetition shuffles.** From iteration 2, games ended in fourfold
  repetition after about 50 plies: lion passes and a piece shuffling back
  and forth. Once a side's root value is below 0.5, the in-tree
  repetition draw (0.5) looks better than playing on, and greedy move
  choice after ply 20 never breaks the loop. **Fix:** sampling throughout
  the game at root temperature 1.25 (`--temp-plies -1 --root-policy-temp
  1.25`), the setting chushogi chose for its later runs.
* **The value head memorized games.** The training value loss fell to
  0.03, but on unseen games (`heldout_loss.py`) the value cross-entropy
  was 3.6 against 0.86 for always predicting the average result. On
  decisive games it called the winner worse than a coin flip (1.84 vs
  0.69). The cause is the ratio of training samples per game: a game's
  ~1,000 positions share one result, and the loop showed each game about
  1,200 times. In an offline test on the same data, 80, 200 and 650 steps
  all generalized at chance or worse, getting worse with more steps.
  Value target `blend` did not fix it.

### run2: playout cap randomization, fewer training samples per game

* **`--full-prob 0.25 --fast-sims 32`.** A quarter of the moves get the
  full 128-simulation search and are recorded; the rest use 32
  unrecorded simulations. That averages 56 simulations per move, about
  2.3 times more games per GPU-hour (~100 plies/s), and records 25% of
  the positions.
* **64 games then 60 steps of 256 per iteration.** That is about 240
  training samples per game, close to KataGo's ratio, instead of ~1,200.
* **Unchanged from step 3:** the 256 × 6 network, value target z, the
  learning rate (cosine decay over 100,000 steps here) and the
  1,500-ply cap.

After 3 iterations the value head is calibrated on unseen games. Its value
cross-entropy is 0.82 against the 0.86 baseline, and it predicts winners at
chance (0.698 vs 0.693), so it is honest but does not know much yet. The
policy is learning: 3.98 against 4.48 for a uniform prior.

**The search walked into king captures.** Against the random player, the
run2 net at step 200 scored 0.5/10, losing nine games by royal capture. It
left its last royal capturable on 15 to 235 of its moves per game. The
cause is in the search, not the encoding: in a plain 5-move king-and-rook
position, 128 simulations with a uniform prior chose a move onto the
rook's file, and every root move had q = 0.5. Inside the tree the FPU
reduction makes untried replies look worse than tried ones. So among ~20
equally likely replies, the opponent's one king capture is often never
tried, and the hanging move keeps the value of a quiet one. Chushogi
covered this with its mate probes, which were left out here.

**Fix:**
* A position where the side to move can capture the opponent's last
  royal is now a proven win in the tree.
* The root plays such a capture at once.
* Proven-losing root moves are dropped from the policy target and from
  move choice.

With the same network, AZ then drew all 7 finished games against the
random player at the cap instead of losing. It no longer blunders its
king, but does not yet attack. run2 and the vs-random numbers above
predate this fix.

### run3-nomate: run2's configuration with the fixed search

run2 was stopped after iteration 4 (46 of 53 games at the cap). The first
run3, now `az/data/run3-nomate`, started from scratch with the same command
and the new search. Iterations 0–1 ended almost entirely by royal capture
(64/64, then 62/64; mean 458 and 784 plies). The cap share then grew to
42, 55 and 63 of 64 games in iterations 2–4, so the drift is not caused by
the king-hanging bug. Capped games end with about 108 of 130 pieces still
on the board: both sides shuffle without trading, rather than reaching a
dead-drawn ending.

### run3: with the mate solver

`az/data/run3`, started from scratch: run3-nomate's command plus
`--mate-nodes 5000 --probe-depth 3 --probe-nodes 2000` (also the new
`run_loop.py` defaults), which also apply to the strength tournaments.

| Iteration | run3: mean plies, games at the cap | run3-nomate |
|---|---|---|
| 0 | 140, 0 | 458, 0 |
| 1 | 523, 0 | 784, 2 |
| 2 | 1,351, 27 | 1,463, 42 |
| 3 | 1,242, 24 | 1,432, 55 |
| 4 | 1,480, 60 | 1,486, 63 |

The solver slowed the drift to the cap but did not stop it. Proven mates
are rare in self-play: 1–6 root-mate moves and 200–490 probe mates per
iteration, from 4–5 million probes. Once both sides stop leaving their
king open, check-only mates within 3 plies barely occur.

**Open issue: draws at the game cap.** In all three runs, games get longer as
soon as the net learns not to hang its king. In run2, 2, 12 and then 49 of
64 games reached the 1,500-ply cap in iterations 0–2. Cap games are the
most expensive (1,500 plies each), and their positions only teach "draw".
The strength tournaments (`--strength-every 10`) and `eval_match.py`
against the random player show whether play still improves. If it does
not, the options are:
* a lower cap (cheaper games, but still draws);
* adjudicating capped games, for example by material, as a training
  label only. That adds domain knowledge, and it is a design choice to
  make deliberately.

**Outcome (stopped after iteration 86).** From iteration 4 on, 46–62 of
each iteration's 64 games reached the cap. Capped games kept about 121 of
the 130 pieces after 1,500 plies; random-vs-random games end by royal
capture in 350–650 plies. The value target was "draw" almost everywhere
(value loss about 0.1), so the search had no reason to attack. All strength
tournaments (iterations 10–80) were draws by repetition, and so they show
nothing. Against the random player, `iter_0049` scored 6.5/10 at 128
simulations and 8/10 at 1,024, and drew all 16 games against the anchor
at 1,024.

### run4: capped games scored by material

`az/data/run4` started from run3's last net with `--cap-score-max 0.5
--cap-score-scale 0.1` and otherwise run3's command. Its 40 iterations used
60 training steps each; it was resumed at iteration 46 with `--train-steps
240`. A capped game is still a draw under the rules and in matches. Only its
value target changes (`az/material.py`):

    d = (material(Sente) - material(Gote)) / material of one side at the start
    t = 0.5 * tanh(d / 0.1)        (Sente's view; Gote's is -t)
    WDL target = [t, 1 - t, 0] if t >= 0 else [0, 1 + t, -t]

Piece values are HaChu's dai shogi table (H.G. Muller's engine, `hachu.c`
0.21): `daiPieces[]` for the eight dai-only pieces, `chuPieces[]` for the
rest, with a promoted piece valued as the piece it becomes (lion 1000, free
king 600, rook 300, gold 151, pawn 40). In run3's capped games the median
gap is 4.5% of an army (t = 0.21), and 12.6% at the 90th percentile
(t = 0.43). Self-play records now store the final position (`"final"`).
Older records have none, so their capped games stay draws.

After 40 iterations: 50 of 64 games capped per iteration (run3's last 20:
55), 118.5 pieces left at the cap, value loss about 0.6. Against random
(60 games at 128 simulations, seeds 41/51/52, `az/data/run4_eval`), the
start net scored 45.5/60 with 31 royal captures and `iter_0040` scored
42/60 with 24 (Fisher p = 0.27, so no measurable change). In the capped
games against random, both nets ended about 45% of an army behind on
material: the agent does not keep its pieces, while the random player
captures and promotes freely.

### Puzzle-training experiments

`tools/puzzlegen` (`make tools/puzzlegen`) finds forced mates of at most 7
plies, the royal capture included (a tsume 1-move mate is 3 plies). It
lists every first move that keeps the shortest mate. With `--contrast` it
lists positions that have a checking move but no such mate.

`az/make_puzzles.py` makes puzzles from real self-play positions: it strips
2–8 defenders within 3 squares of the defender's single royal and moves 2–5
attackers to within 4 squares. About 0.7% of the candidates are genuine 3–7
ply mates. With `--contrast CHECKPOINT` it writes contrast positions, labelled
by a search of that net. `az/train.py --puzzle-dirs ... --puzzle-fraction F`
mixes them into each batch, and `--puzzle-policy-weight 0` makes them
value-only. `az/puzzle_eval.py` measures a raw net on a puzzle file.

Real games contain almost no short mates. Only 2 of 3,000 positions from
iterations 40–49 had a mate within 7 plies. The root solver plays nearly
all of the mates that do occur: 97.7% were solved within its 5,000 nodes.

A/B test (`az/data/puzzle_exp`): run3's `iter_0076` was fine-tuned for 540
steps on iterations 77–85. Arm A had no puzzles. B had 10% puzzles with
policy and value targets. C had 10% value-only puzzles. D was C plus 5%
contrast positions. Each arm then played 60 games against random at 128
simulations.

| Arm | vs random | Royal captures | P(win), held-out puzzles | P(win), contrast (no mate) | Best move a check, contrast |
|---|---|---|---|---|---|
| A | 41.5/60 | 27 | 0.02 | 0.02 | 0.1% |
| B | 50.5/60 | 41 (p = 0.016) | 0.97–0.99 | 0.97 | 51.5% |
| C | 45.5/60 | 31 (p = 0.58) | 0.95–0.99 | 0.97 | 0.1% |
| D | 40.5/60 | 27 | 0.49–0.68 | 0.35 | 0.1% |

* Only the policy part helps against random. Value-only puzzles do not.
* B and C call positions that merely look like puzzles won, whether or not a
  mate exists. B also prefers checks there, with no mate to justify them.
  In ordinary game positions, B's preference for checks only rises to the
  search's own level: 1.8% of the policy on checks, against 1.7% in the
  visit target and 0.8% for A.
* Transfer to real mates from game endings is small for every arm.
* B's gain may partly reflect the random opponent, which leaves its king
  exposed and so creates puzzle-like positions. A defending opponent is
  needed to tell.

## Open points

1. **Draws at the game cap and material.** Is run4's material scoring,
   with 240 training steps per iteration, enough? Compare with its start
   net against random again (`az/data/run4_eval/cmp40.sh`, after changing
   the checkpoint number).
2. **Mate-solver settings.** run3 uses chushogi's 5000 / 3 / 2000. The
   probes cost no measurable self-play throughput (5.7k evaluations/s with
   the solver, 5.5k without; the CPU work hides behind the GPU), so a
   deeper probe or larger root budget is affordable if mates matter.
3. **Throughput.** The evaluator is memory-bound. A second eval server
   (`--eval-servers 2`) or fusing the elementwise ops are untested.
   `torch.compile` was slower in a quick test.
