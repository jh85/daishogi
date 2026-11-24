# Rules implemented, and how they were decided

## Sources and precedence

1. **Shogitter's dai shogi rule (ID 49)**: the
   [rule page](https://shogitter.com/rule/%E5%A4%A7%E5%B0%86%E6%A3%8B) and
   the [piece page](https://shogitter.com/rule/%E5%A4%A7%E5%B0%86%E6%A3%8B/koma).
2. **haitaka-variants**, Shogitter's own engine for that rule. This is the
   `daishogi` feature at commit `6222835` in `../haitaka-variants`, file
   `haitaka/src/extended/classical.rs` with tables in `classical/defs.rs`.
   It is used as an executable specification. Its own verification notes
   say it matches Shogitter's TypeScript rules on all legal moves and
   transitions.
3. Japanese and English Wikipedia, for cross-checking the setup and the
   piece movements.

Where the written sources are silent or ambiguous, this implementation
follows the reference behaviour exactly, so that the comparison tests are
meaningful. Each place where the reference's behaviour could be read
differently from the written rules is listed under
[Discrepancies and choices](#discrepancies-and-choices). There are three
deliberate exceptions:
* **Promotion** follows chu shogi by default (discrepancies 2 and 3). The
  reference's rule is available as `--promotion normal`, and every
  comparison with the reference uses it.
* Two concern lion-versus-lion captures by a two-step move, where this
  implementation follows lishogi's chu shogi and the Japan Chu Shogi
  Association (JCSA):
  * a lion adjacent to the start square may always be taken, also on the
    second step (discrepancy 5);
  * a pawn or go-between taken on the first step no longer blocks a
    protector (discrepancy 9).

## Board and setup

The board is 15×15 and each side has 65 pieces of 29 kinds; the setup is the
diagram in [FORMAT.md](FORMAT.md). All sources agree on it, including two
easily-swapped details:

* The **kirin** stands on Sente's left of the lion (9m) and the **phoenix**
  on its right (7m). Gote's setup is the 180° rotation.
* The **go-betweens** stand on files 5 and 11, in front of the pawns.

There are **no drops**. A captured piece leaves the game.

## Piece movements

Notation: "step" moves one square. "jump" goes to an exact square, and
pieces in between do not block it. "range" slides any distance until it is
blocked. "range 2" slides at most two squares and is blocked by a piece on
the first square. Directions are from the moving side's point of view.

| Piece | Movement |
|---|---|
| King `KI`, Prince `+DE` | step in all 8 directions |
| Gold `GO` and every gold-moving promotion (`+PA +KN +IR +ST +EW +AB +CS +VO +FD`) | step forward, forward-diagonal, sideways, back |
| Silver `SI` | step forward, forward-diagonal, back-diagonal |
| Copper `CO` | step forward, forward-diagonal, back |
| Iron `IR` | step forward, forward-diagonal |
| Stone `ST` | step forward-diagonal |
| Evil wolf `EW` | step forward, forward-diagonal, sideways |
| Angry boar `AB` | step orthogonally |
| Cat sword `CS` | step diagonally |
| Ferocious leopard `FL` | step forward, back, all diagonals |
| Blind tiger `BT` | step in all directions except straight forward |
| Drunk elephant `DE`, `+GB` | step in all directions except straight back |
| Go-between `GB` | step forward, back |
| Pawn `PA` | step forward |
| Knight `KN` | jump to the two squares two forward and one sideways |
| Kirin `KR` | step diagonally; jump two squares orthogonally |
| Phoenix `PH` | step orthogonally; jump two squares diagonally |
| Lance `LA` | range forward |
| Reverse chariot `RC` | range forward and back |
| Side mover `SM`, `+CO` | range sideways; step forward and back |
| Vertical mover `VM`, `+SI` | range forward and back; step sideways |
| Rook `RO`, `+GO` | range orthogonally |
| Bishop `BI`, `+FL` | range diagonally |
| Dragon king `DK`, `+RO` | range orthogonally; step diagonally |
| Dragon horse `DH`, `+BI` | range diagonally; step orthogonally |
| Free king `FK`, `+PH` | range in all 8 directions |
| Violent ox `VO` | range 2 orthogonally |
| Flying dragon `FD` | range 2 diagonally |
| White horse `+LA` | range forward, forward-diagonal, back |
| Whale `+RC` | range forward, back, back-diagonal |
| Flying stag `+BT` | range forward and back; step sideways and diagonally |
| Free boar `+SM` | range sideways and diagonally |
| Flying ox `+VM` | range forward, back and diagonally |
| Lion `LI`, `+KR` | any square within two (jumps), or two king steps (see below) |
| Horned falcon `+DH` | range in all directions except forward; lion power straight forward |
| Soaring eagle `+DK` | range orthogonally and back-diagonally; lion power on both forward diagonals |

### Lion power (two-step moves)

* The **lion** may take one king step and then a second king step in any
  direction, capturing on either square. A capture on the first square
  followed by a step back to the start square is igui (居喰い). The same
  out-and-back move over an empty square is a pass. The squares at distance
  two are also reachable by a direct jump.
* The **horned falcon** has the same power along the forward line only. It
  can step 1 forward and then 1 more forward or back, and it can jump 2
  forward.
* The **soaring eagle** has the same power along each forward diagonal.
* The first step may not land on a friendly piece. The second step may not
  end on a friendly piece other than the vacated start square.

## Promotion

The promotion zone is the far **five ranks**. Promoted pieces never
promote again. The king, lion and free king do not promote. Promotion takes
effect after the move, so the whole move uses the unpromoted piece's
movement. (Only lions, falcons and eagles make two-step moves, and none of
them can promote.)

Two rules are implemented. The default is the chu rule.

**Chu rule** (default, `--promotion chu`). A promotable piece **may**
promote on a move that:
* **enters** the zone from outside;
* **starts** in the zone and **captures** an enemy piece, including a move
  that leaves the zone to capture outside it;
* ends where the unpromoted piece could **never move again**: a pawn,
  lance, iron or stone general on the **last rank**, or a knight on the
  **last two ranks**.

Every other move may not promote. In particular:
* a move that leaves the zone without capturing may not promote;
* a piece that entered the zone without promoting may not promote later by
  moving inside it without a capture. It may promote again only by
  capturing, by leaving and re-entering, or by reaching a rank where it
  could never move again.

Promotion is **never compulsory**. A piece that does not promote where it
can never move again stays there as a **dead piece** with no legal moves,
and it can still be captured.

The rule needs no memory of earlier moves. Whether a move may promote
depends only on its start and end squares and whether it captures.

**Normal rule** (`--promotion normal`, haitaka-variants and Shogitter, which
call it "Normal"). A promotable piece **may** promote at the end of any move
that **starts or ends** in the zone. That includes moves inside the zone
without a capture, and moves that leave it. Promotion is **compulsory** where
the unpromoted piece could never move again, the same pieces and ranks as
above, so there are no dead pieces.

| Move (examples in `testPromotionRule`, `tests/unit_tests.cpp`) | Chu | Normal |
|---|---|---|
| Enters the zone | may promote | may promote |
| Starts in the zone and captures (also outside it) | may promote | may promote |
| Inside the zone, or leaving it, without a capture | **no** | may promote |
| Pawn, lance, iron or stone to the last rank, or knight to the last two (also inside the zone without a capture) | may promote (if not, the piece is dead) | **must** promote |

* **Runtime:** `daishogi --promotion normal ...` and
  `daishogi-az --promotion normal` set the rule for the process. Self-play
  records name it (`"promotion"`).
* **Compile time:** `make clean && make PROMOTION=normal` changes the
  default.
* The comparison tests against the reference (`tests/compare.py`,
  `replay_games.py`, `live_compare.py`, `mutation_check.py`) pass
  `--promotion normal` (`REF_RULES` in `tests/refconv.py`). Under the chu
  rule, 4,427 of the corpus's 14,251 positions have different legal moves.
* Promotion targets are listed in [FORMAT.md](FORMAT.md). They match
  Shogitter and both Wikipedias.

## Captures involving lions

"Lion" here means `LI` and `+KR` (a kirin promoted to a lion). The horned
falcon and soaring eagle are *not* lions, although they have lion power.

1. **Lion takes an adjacent lion**: always allowed
   (隣接する獅子どうしの取り合い). "Adjacent" means adjacent to the square
   where the move starts, so this also covers the second step of a two-step
   move, through an empty square or a pawn or go-between (`8h8g7h`). A lion
   that only becomes adjacent after the first step is not covered (rule 3).
   This is lishogi's rule; the reference applies it to single steps only
   (discrepancy 5).
2. **Lion takes a non-adjacent lion by a jump**: allowed only if that lion
   is not protected (足のある獅子は獅子で取れない).
   * Protection is judged with the capturing lion lifted from its square. A
     slider of the defending side that reaches the target *through* the
     capturing lion's square therefore counts (獅子かげの足).
3. **Lion takes a non-adjacent lion on the second step of a two-step
   move** (the target is two squares from the start square):
   * If the first step captured a piece other than a pawn or go-between,
     the capture is always allowed (付け喰い).
   * Otherwise, i.e. the first step was empty or captured a pawn or
     go-between, it is allowed only if the target lion is not protected.
     Protection is judged with the capturing lion lifted.
   * A pawn or go-between taken on the first step is removed for this
     judgement as a *blocker*: a protector whose line it blocked counts
     (`8h7g6f` with Gote's pawn on 7g, lion on 6f and bishop on 10j is
     illegal). It still counts as a *protector* itself: a lion protected
     only by the go-between standing between the two lions cannot be taken
     by taking both (JCSA 2004 supplementary rules, case 4). This is
     lishogi's rule; the reference differs (discrepancy 9).
4. **先獅子 (counter-strike rule)**: if the last hop of the opponent's
   previous move was a non-lion capturing a lion, then on this move no
   non-lion piece may capture any lion, anywhere. Lions are not restricted
   by it. A lion capturing a lion never starts it, so after 付け喰い the
   opponent may take back.
5. **Horned falcon / soaring eagle two-step moves** are judged hop by hop,
   with the 先獅子 state updated after the first hop:
   * the first hop may take a lion only if 先獅子 is off;
   * the second hop may take a lion only if the first hop did not take one.

The SFEN's third field records whether 先獅子 is active. It is set after
every move from that move's **last** hop.

### Counter-strike (先獅子) variants

The rule above protects **every** lion. JCSA and lishogi use variants of it
that protect fewer lions. Both are available for comparison with chu shogi.

| Variant | `--counter-strike` | Protected while 先獅子 is on | Source |
|---|---|---|---|
| All (default) | `all` | every lion | haitaka-variants / Shogitter rule 49 |
| Lishogi | `lishogi` | every lion except one on the square of the capture, i.e. a kirin that took the lion there and promoted | lishogi (`../chushogi`, `RuleSet::Lishogi`) |
| JCSA | `jcsa` | only a defended lion (足のある獅子), judged with the capturer lifted so that x-rays through it count | JCSA 2019 regulations 七.四 (`../chushogi`, `RuleSet::JCSA`) |

* **Runtime:** `daishogi --counter-strike NAME ...` and
  `daishogi-az --counter-strike NAME` set the rule for the process.
  Self-play records name it (`"counter_strike"`).
* **Compile time:** `make clean && make COUNTER_STRIKE=lishogi` (or `jcsa`)
  changes the default. The switch costs nothing measurable: perft 4
  takes 2.48 s either way.
* **The ban's other parts are the same in every variant.** Only the
  protection condition changes:
  * Lions are never restricted by it.
  * The flag is set from the last hop.
  * Falcon and eagle two-step moves are judged hop by hop (item 5 above).
    A falcon that takes a lion on its first step may take a second lion
    on its second step only if the variant leaves that lion unprotected,
    i.e. under JCSA, when it is undefended.
  * Lishogi itself (scalashogi `Chushogi.scala` and `MoveActor.scala`,
    commit c3886687ac) handles falcon and eagle double moves differently
    from the reference, and so from every variant here:
    * it checks both steps against the ban as it stood before the move;
    * it sets the ban from the last step that took a lion, or else from
      the mid step.

    So, during the ban, a lion on the second step is protected even when
    the first step took none. Without the ban, a falcon may take two
    lions. Taking a lion on the first step and returning starts the ban.
    `../chushogi`'s lishogi mode matches lishogi on all three points since
    its commit ea6f874. That commit applied lishogi's fix fcf0664 ("Prevent
    illegal lion recapture in midstep", 2026-10-23); before it, the mid step
    could take a protected lion during the ban.
* **Repetition:** under `lishogi` the exact capture square changes which
  moves are legal, so it becomes part of the position for the repetition
  rule (`Position::sameState`, `key`).
* **On the reference corpus,** `lishogi` adds legal moves in 7 of 14,251
  positions and `jcsa` in 125. Neither ever removes one. `make test`
  checks the default only.

## Game end

* **Royal capture**: the king and prince are royal. A player who captures
  all of the opponent's royal pieces wins at once (王将、太子すべてを取れば
  勝ち). If the king is captured while a prince remains, the game goes on.
  If the first step of a two-step move takes the last royal piece, the
  move ends there: no second step is offered.
* **No check rule**: a royal may stay in or move into attack (王手放置を
  許容). Legal moves are never filtered for king safety.
* **No legal move**: the side to move loses. Shogitter describes this for
  stalemate ("not in check and no move"). With no check rule, being in
  check makes no difference, and the reference treats every no-move
  position as a loss. In practice it needs pieces boxed in by their own
  side; the test corpus has such a position.
* **Repetition**: when a position occurs for the **fourth** time, the game
  is a draw. Position here means board, side to move and the 先獅子 flag;
  the move number does not count.
  * Exception (perpetual check): suppose that, since the first occurrence,
    a side V had one of its royal pieces attacked in *every* position with V
    to move. Then V wins.
* **Move limit** (`play --max-plies`): a test cutoff only. It is reported
  as "cutoff", not as a draw or any other result.

## Restrictions

* **Two pawns / two go-betweens on a file (二歩・二仲人)**: after a move, a
  side may not have more than one unpromoted pawn, or more than one
  unpromoted go-between, on the destination file. This comes from
  Shogitter, and the reference applies it to the moving piece. Pawns and
  go-betweens never change file, so it can never trigger from the initial
  position. It matters only for set-up positions, where it is enforced
  exactly like the reference: the moving piece may still promote, if the
  promotion rule allows that move to promote.

## Discrepancies and choices

1. **Lion capture rules at all.** English Wikipedia says the chu shogi lion
   rules do not apply in dai shogi. Japanese Wikipedia treats them as
   hypothetical. Shogitter lists them explicitly (獅子取りの特例), and the
   reference implements them. **Implemented: Shogitter's.** Shogitter is the
   rule authority for this project, and without the rules the reference
   comparison would fail on every lion fight.
2. **Promotion conditions.** English Wikipedia describes chu-style
   conditions: promote on entering the zone, otherwise only when capturing
   inside it. Japanese Wikipedia notes that internet programs use the
   honshogi style. Shogitter and the reference use the honshogi ("Normal")
   style: promotion is allowed on any move starting or ending in the
   zone. **Implemented: both. The chu rule is the default; Normal is
   `--promotion normal`** (see [Promotion](#promotion)).
3. **Compulsory promotion.** The Wikipedias only say that unpromoted pieces
   on the last rank(s) would be dead. The reference forbids dead pieces
   (pawn, lance, iron and stone on the last rank; knight on the last two),
   so promotion is compulsory there. **Implemented: never compulsory under
   the chu rule, as in chu shogi.** A piece that does not promote stays
   there, dead, and can be captured. Under `--promotion normal` it is
   compulsory, as in the reference.
4. **先獅子 versus falcon and eagle two-step moves.** Read literally, 先獅子
   is about whether a lion was taken by a non-lion on the previous move.
   The reference sets the flag from the **last hop** only, and judges each
   falcon/eagle hop against the flag as updated by the hop before. This
   gives three edge cases:
   * A falcon or eagle that takes a lion on its first step and then steps
     on or returns (`8h8g8f`, `8h8g8h`) does **not** trigger 先獅子. The
     opponent may take a lion with a non-lion at once. Covered by the
     `falcon-*-then-rook-takes-lion` games in `tests/data/rule_games`.
   * While 先獅子 is active, a falcon or eagle may not take a lion on its
     first step. It **may** take one on its second step if the first step
     took no lion (`falcon-senjishi-second-step`).
   * A falcon or eagle that takes a lion on its first step may not take a
     second lion on its second step, even when 先獅子 is off
     (`falcon-two-lions`).

   **Implemented: the reference behaviour**, for exact comparability. These
   rules concern pieces that exist only after promotion, plus a lion, in a
   precise configuration. They are isolated in `secondHopAllowed()` and
   `Position::apply()` and can be changed in a few lines if a stricter
   reading is wanted.
5. **Lion two-step capture of a lion adjacent to its start square.** E.g.
   Sente lion 8h, Gote pawn 8g, Gote lion 7h protected by a Gote gold on
   6h. The single step `8h7h` is legal everywhere. For the two-step forms
   (`8h8g7h` through the pawn, `8h7g7h` through an empty square):
   * The reference (`classical_capture_allowed` in
     `haitaka/src/extended/classical.rs`, and the same code in its chu
     shogi, `chu.rs`) applies the adjacent-lion exemption to single steps
     only. A second step onto a lion goes straight to the protection test,
     so both forms are illegal here.
   * lishogi (scalashogi `Chushogi.lionMoveFilter`) tests
     `d.dist(a.square) == 1` first: a target adjacent to the start square
     is always allowed. `../chushogi` does the same.
   * JCSA: the 2019 regulations, lion rule 1, say adjacent lions may
     always be captured by a lion. In the 2004 supplement's counter-example
     to case 4, a lion takes a pawn and then a protected lion adjacent to
     the pawn's square but not to its own: illegal, "one move, not two".
     Adjacency is therefore judged from where the move starts, which
     makes `8h8g7h` legal. No JCSA diagram shows this exact position.

   **Implemented: lishogi's rule.** A lion may always take an adjacent
   lion. Reported upstream as
   [haitaka-variants#44](https://github.com/na2hiro/haitaka-variants/issues/44).
   Through an empty square, the two-step form reaches the same position as
   the single step (see 8); through a pawn or go-between it also removes
   that piece.

   Testing: `tools/refgen` adds every such two-step move missing from the
   reference's list. It derives the move's result by playing the single
   step on the board with the first-step victim removed, so the position
   reached is still checked against the reference. In the committed corpus
   this adds 154 moves in 63 positions (9 of them from reference-played
   games), mostly through an empty square.
6. **Igui on the last royal piece.** The reference ends a move as soon as
   its first step takes the opponent's last royal piece. `8h8g8h` on a lone
   king is therefore not a legal move, and `8h8g` captures it.
   **Implemented: the same.** The game result does not change.
7. **Repetition.** The written sources say nothing about repetition for dai
   shogi. Fourfold repetition as a draw, with the perpetually-checking side
   losing, is the reference's (Shogitter's) rule, and it is what is
   implemented. It matches common chu shogi practice.
8. **Redundant lion paths.** A lion move through an empty square that
   lands where a direct jump or step also lands (`8h8g8f` and `8h8f`;
   `8h8g7h` and `8h7h`) appears more than once in the legal-move list, as
   separate move strings. The reference and lishogi's chu generator do the
   same (lishogi also for the adjacent-lion capture of 5). A random player
   therefore picks those destinations a little more often than if they
   were merged.
9. **A pawn or go-between taken before a lion: blocker or not?** A lion
   takes a pawn or go-between on its first step and a lion on its second,
   and a defender's line to that lion ran through the captured piece, e.g.
   Sente lion 8h, Gote pawn 7g, Gote lion 6f, Gote bishop 10j (`8h7g6f`).
   * The reference judges protection on the board before the move with
     only the capturing lion lifted, so the captured pawn still blocks the
     bishop and the move is legal.
   * lishogi (scalashogi `Chushogi.lionMoveFilter`, also
     `../chushogi/src/movegen.cpp`) empties the first-step square as well,
     so the bishop protects 6f and the move is illegal. It separately keeps
     the captured pawn or go-between as a protector, which matches JCSA's
     2004 supplementary rules, case 4.
   * No written source rules on this exact position. The protection rule
     asks whether the capturing lion could be taken back after the move;
     here the bishop can. The pawn exception to 付け喰い is meant to stop a
     pawn from enabling the capture of a protected lion, and the
     reference's reading does the opposite: with 7g empty `8h7g6f` is
     illegal everywhere, and adding a Gote pawn on 7g makes it legal.

   **Implemented: lishogi's rule.** Reported upstream (as a proposal) as
   [haitaka-variants#45](https://github.com/na2hiro/haitaka-variants/issues/45).
   A target adjacent to the start square is covered by 5 instead: it may
   always be taken. The single jump `8h6f` stays legal, because there the
   pawn really stays on 7g.

   Testing: `tools/refgen` asks the reference again for each lion move that
   takes a pawn or go-between and then a non-adjacent lion, with that piece
   removed, which is exactly lishogi's judgement. Moves that fail are
   excluded from the corpus's expected moves and listed as
   `reference_only`: 4 moves, all in the hand-written cases. With the
   corpus seed it came up once in the reference-played games (before
   rule 5 was adopted).

   **How both differences are tested.** "Our moves" are the reference's
   moves minus the forbidden ones (9) plus the added ones (5), both derived
   from the reference for each position. The corpus lists them as
   `reference_only` and `ours_only`. Reference-played games pick from our
   moves, reference perft counts them, and the reference replays our games
   accepting them. An added move captures a lion, so no earlier position
   can recur. The tool can therefore continue from the resulting position
   without history, and repetition results are unaffected. Every other
   move, transition and result must still match the reference exactly.

## Known limitations

* The engine is a reference-quality move generator, not a playing engine:
  there is no search, no evaluation and no USI protocol. The `batch`
  protocol is for scripting.
* The move generator is simple mailbox code without bitboards. It runs
  perft at about 10 million nodes per second and finishes about 50 random
  games per second, which is enough for testing and self-play
  experiments.
* `perft` does not apply repetition. A fourth occurrence needs at least six
  plies, so perft agrees with a repetition-aware count up to depth 6.
* A SFEN carries no game history. Repetition is judged only over moves
  played through `play`, `position ... moves` or `Game`.
* The SFEN parser does not enforce per-side piece counts. The reference
  does, so positions with, say, three knights per side can be analysed
  here but not by the reference.
