# Dai shogi position and move formats

The formats extend the SFEN/USI conventions used for chu shogi by lishogi
and by `../chushogi` (file number + rank letter squares, `+` for promoted
pieces, a third SFEN field for the lion-capture state). The changes are a
15×15 board and two-letter piece codes, because dai shogi has more piece
kinds (29) than the alphabet has letters.

## Board coordinates

```
      15  14  13  12  11  10   9   8   7   6   5   4   3   2   1
   +--------------------------------------------------------------+
   |  la  kn  st  ir  co  si  go  ki  go  si  co  ir  st  kn  la  | a   <- Gote's back rank
   |  rc   .  cs   .  fl   .  bt  de  bt   .  fl   .  cs   .  rc  | b
   |   .  vo   .  ab   .  ew  ph  li  kr  ew   .  ab   .  vo   .  | c
   |  ro  fd  sm  vm  bi  dh  dk  fk  dk  dh  bi  vm  sm  fd  ro  | d
   |  pa  pa  pa  pa  pa  pa  pa  pa  pa  pa  pa  pa  pa  pa  pa  | e
   |   .   .   .   .  gb   .   .   .   .   .  gb   .   .   .   .  | f
   |   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .  | g
   |   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .  | h
   |   .   .   .   .   .   .   .   .   .   .   .   .   .   .   .  | i
   |   .   .   .   .  GB   .   .   .   .   .  GB   .   .   .   .  | j
   |  PA  PA  PA  PA  PA  PA  PA  PA  PA  PA  PA  PA  PA  PA  PA  | k
   |  RO  FD  SM  VM  BI  DH  DK  FK  DK  DH  BI  VM  SM  FD  RO  | l
   |   .  VO   .  AB   .  EW  KR  LI  PH  EW   .  AB   .  VO   .  | m
   |  RC   .  CS   .  FL   .  BT  DE  BT   .  FL   .  CS   .  RC  | n
   |  LA  KN  ST  IR  CO  SI  GO  KI  GO  SI  CO  IR  ST  KN  LA  | o   <- Sente's back rank
   +--------------------------------------------------------------+
```

* **Files** are numbered 1–15 from Sente's right to Sente's left, as in
  Japanese notation. **Ranks** are the letters `a`–`o`, from Gote's back
  rank (top) to Sente's back rank (bottom). Rank `a` is 一段 and rank `o` is
  十五段.
* A **square** is written as the file number followed by the rank letter.
  Examples: `8o` is Sente's king, `1a` is the top-right corner, and `15o` is
  the bottom-left corner.
* **Sente** (先手, black, moves first) starts at the bottom and moves towards
  rank `a`. **Gote** (後手, white) starts at the top.
* Internally a square is the index `y*15 + x`, with `x = file-1` and
  `y = rank letter - 'a'`.

## Piece codes

A piece code is **exactly two letters**, both uppercase for **Sente** and
both lowercase for **Gote** (`GO` / `go`). A mixed-case pair such as `Go` is
rejected. A **promoted** piece is written as `+` followed by the code of the
piece it promoted from. This keeps the origin of the piece, so `+KN` (gold
from knight) and `+AB` (gold from angry boar) are different pieces, as they
are in the reference.

The codes are chosen by these rules, so that they can be extended to the
larger variants (tenjiku, dai-dai, maka-dai-dai, tai, taikyoku shogi) without
renaming:

1. two-word names: the initials (Dragon horse `DH`, Free king `FK`);
2. one-word names: the first two letters (Lance `LA`, Lion `LI`);
3. "X general": the first two letters of X (Gold `GO`, Iron `IR`), because
   `GG` belongs to the great general (大将) of the larger variants;
4. a clash goes to the more common piece; the other one takes its first
   letter and the next distinctive consonant (King `KI`, Kirin `KR`).

| Code | Piece | 日本語 | Promotes to | haitaka id |
|---|---|---|---|---|
| `PA` | Pawn | 歩兵 | `+PA` gold (と金) | aa |
| `LA` | Lance | 香車 | `+LA` white horse 白駒 | ab |
| `KN` | Knight | 桂馬 | `+KN` gold | ac |
| `SI` | Silver general | 銀将 | `+SI` vertical mover | ad |
| `GO` | Gold general | 金将 | `+GO` rook | ae |
| `BI` | Bishop | 角行 | `+BI` dragon horse | af |
| `RO` | Rook | 飛車 | `+RO` dragon king | ag |
| `KI` | King | 玉将 / 王将 | — | ah |
| `DH` | Dragon horse | 龍馬 | `+DH` horned falcon 角鷹 | am |
| `DK` | Dragon king | 龍王 | `+DK` soaring eagle 飛鷲 | an |
| `LI` | Lion | 獅子 | — | bl |
| `DE` | Drunk elephant | 醉象 | `+DE` prince 太子 | cf |
| `CO` | Copper general | 銅将 | `+CO` side mover | ci |
| `FL` | Ferocious leopard | 猛豹 | `+FL` bishop | cl |
| `RC` | Reverse chariot | 反車 | `+RC` whale 鯨鯢 | cm |
| `BT` | Blind tiger | 盲虎 | `+BT` flying stag 飛鹿 | cn |
| `KR` | Kirin | 麒麟 | `+KR` lion | co |
| `PH` | Phoenix | 鳳凰 | `+PH` free king | cp |
| `GB` | Go-between | 仲人 | `+GB` drunk elephant | cq |
| `SM` | Side mover | 横行 | `+SM` free boar 奔猪 | cr |
| `VM` | Vertical mover | 竪行 | `+VM` flying ox 飛牛 | cs |
| `FK` | Free king | 奔王 | — | ct |
| `IR` | Iron general | 鉄将 | `+IR` gold | dp |
| `ST` | Stone general | 石将 | `+ST` gold | dq |
| `EW` | Evil wolf | 悪狼 | `+EW` gold | dr |
| `AB` | Angry boar | 嗔猪 | `+AB` gold | ds |
| `CS` | Cat sword | 猫刄 | `+CS` gold | dt |
| `VO` | Violent ox | 猛牛 | `+VO` gold | du |
| `FD` | Flying dragon | 飛龍 | `+FD` gold | dv |

Each promoted code stands for one reference piece:

| Code | haitaka id | Code | haitaka id |
|---|---|---|---|
| `+PA` | ai | `+BT` | cw |
| `+LA` | cu | `+KR` | dh |
| `+KN` | do (origin ac) | `+PH` | dj |
| `+SI` | de | `+GB` | dk |
| `+GO` | dd | `+SM` | cx |
| `+BI` | dl | `+VM` | cy |
| `+RO` | dm | `+IR` | do (origin dp) |
| `+DH` | cz | `+ST` | do (origin dq) |
| `+DK` | db | `+EW` | do (origin dr) |
| `+DE` | cg | `+AB` | do (origin ds) |
| `+CO` | df | `+CS` | do (origin dt) |
| `+FL` | dg | `+VO` | do (origin du) |
| `+RC` | cv | `+FD` | do (origin dv) |

`KI`, `LI` and `FK` and all promoted pieces do not promote. A code such as
`+KI` or `++PA` is rejected.

Reserved for the larger variants, where these pieces also exist unpromoted
(in dai shogi they appear only as promoted pieces, written as above): `WH`
white horse 白駒, `WL` whale 鯨鯢, `HF` horned falcon 角鷹, `SE` soaring eagle
飛鷲, `CP` crown prince 太子, `FS` flying stag 飛鹿, `FB` free boar 奔猪, `FO`
flying ox 飛牛.

## SFEN

```
<board> <side to move> <lion-capture square> <move number>
```

1. **Board**: 15 ranks from `a` to `o`, separated by `/`. Each rank lists
   files 15 → 1, i.e. left to right as in the diagram. A run of empty squares
   is a decimal number from 1 to 15. Every rank must add up to exactly 15
   squares.
2. **Side to move**: `b` for Sente or `w` for Gote.
3. **Lion-capture state** (先獅子): `-` if the restriction is off. Otherwise
   it is the square where the previous move's last hop was a non-lion piece
   capturing a lion. While it is set, the side to move may not capture a
   lion with a non-lion piece (see [RULES.md](RULES.md)). Only whether it is
   set matters for the rules; the square is informational. The reference
   stores only a boolean `senjishi`.
4. **Move number**: informational. It is incremented by each move and
   ignored by the rules and by repetition.

Fields 2–4 may be omitted; the defaults are `b - 1`. The parser rejects
unknown codes, wrong rank lengths, and unpromoted pieces on squares from
which they could never move: a pawn, lance, iron or stone general on the last
rank, or a knight on the last two ranks. It does **not** enforce the
reference's piece-count limits per side.

A SFEN contains spaces, so quote it in the shell.

### Initial position

```
laknstircosigokigosicoirstknla/rc1cs1fl1btdebt1fl1cs1rc/1vo1ab1ewphlikrew1ab1vo1/rofdsmvmbidhdkfkdkdhbivmsmfdro/papapapapapapapapapapapapapapa/4gb5gb4/15/15/15/4GB5GB4/PAPAPAPAPAPAPAPAPAPAPAPAPAPAPA/ROFDSMVMBIDHDKFKDKDHBIVMSMFDRO/1VO1AB1EWKRLIPHEW1AB1VO1/RC1CS1FL1BTDEBT1FL1CS1RC/LAKNSTIRCOSIGOKIGOSICOIRSTKNLA b - 1
```

`daishogi` accepts `startpos` for this position.

### Example positions

All of these come from the test corpus. `./daishogi show "<sfen>"` prints a
diagram.

* Middle game after 120 random plies. There are promoted pieces, e.g. a
  horned falcon `+DH` on 8h. Sente to move, 108 legal moves:
  ```
  la1stirco1gokigofl1irlivola/2csdhfl2debtsicostcs1rc/rcvoknabbt2phfkew1abknrofd/ro1sm1vmew1dksidhvm1sm2/1pa1papapapa1pa1papapapa1/pa1pa1gb3dkpabi3pa/2fd5kr6/6bi+DH7/6PA8/2BIPAGB2PA1PAGB2PA1/PAPAPA1PAPADK1PA1PAPAPAVOPA/1RO1VOVMKR1FKDKDHBI1RO2/2FDABSM1EWBTPHEWAB1SMCSFD/RC1CSIRFLLI1DEBTCOFL1IRSTRC/LAKNST1COSIGOKIGOSI1VM1KNLA b - 1
  ```
* Lion-capture restriction active: Gote's lion was just taken on 12c by a
  non-lion. Sente to move, 169 legal moves:
  ```
  1vost2ki4co2knla/la1dkab6fl2strc/rc1roir9vo1/2fl+PH1+BI3kr1sm1abro/9desm3fd/10ew4/pa3papa2pa2BIpaKN1/2pa8DKPApapa/2PA6SIGB4/1VO+bi1+VM4PAPA2PAPA/PA3PA1PA1GO1VO4/1FLAB1GOEW3dh1pa1FD1/RC2CSCORO8RC/LA1SMSTIRSI1BT2FLKR1ABLA/2FD4KIBT6 b 12c 1
  ```
* Endgame. Sente's king has been captured, but its prince (`+DE` on 4i)
  keeps the game going:
  ```
  kr11+GO1+KN/15/2ki2sm9/15/5cs8+PA/15/vopa+DK6PA5/3si11/11+DE3/15/10KR4/+st3+go10/15/5BT9/7FL4BT1VO b - 1
  ```
* Lion versus a protected lion. Sente's lion on 8h may take the lion on 8g,
  because they are adjacent (`8h8g`, `8h8g8h`, `8h8g8f`):
  ```
  14ki/15/15/15/15/7go7/7li7/7LI7/15/15/15/15/15/15/KI14 b - 1
  ```

## Moves (USI style)

| Form | Meaning | Example |
|---|---|---|
| `<from><to>` | ordinary move or capture | `8k8j` (pawn), `13l13g` (side mover) |
| `<from><to>+` | the same move, promoting | `8e8d+` |
| `<from><mid><to>` | two-step move of a lion, horned falcon or soaring eagle: first step to `mid`, second to `to` | `8h8g8f` |
| `<from><mid><from>` | step out and back: igui (居喰い) if `mid` holds an enemy piece, otherwise a pass | `8h8g8h` |

* On input, a trailing `=` (explicit non-promotion) is accepted and
  ignored.
* Two-step moves never promote. The only pieces that can make them are the
  lion, the lion promoted from a kirin, the horned falcon and the soaring
  eagle, and none of them promotes.
* A lion move through an empty square, such as `8h8g8f`, ends where a direct
  jump does (`8h8f`). Both are listed as separate legal moves, as in the
  reference and in lishogi's chu shogi. The capture rules can treat the two
  differently (see [RULES.md](RULES.md)).

## Reference (haitaka-variants) correspondence

* Reference square `[x, y]` (1-based, `x` = file, `y` = rank from Gote's
  side) is our square `<x><letter y>`. The two systems agree square for
  square.
* Reference color 0 is Sente (`b`, uppercase); color 1 is Gote.
* Reference move `{"from":[x,y],"path":[[..],[..]],"promotion":p}` is
  `<from><path squares...>[+]`.
* `state.senjishi == true` is a lion-capture square other than `-`.
* Our piece kind numbers 0–54 (`src/types.h`) equal the reference's
  `PieceId`s. The test harness does not rely on this; it uses its own
  mapping in `tests/refconv.py`.

## Command-line and batch interfaces

```
./daishogi moves "<sfen>"|startpos [usi moves...]   # legal moves after the moves
./daishogi show  "<sfen>"|startpos [usi moves...]   # diagram + SFEN + status
./daishogi perft DEPTH ["<sfen>"|startpos] [--divide]
./daishogi play  [--seed N] [--games N] [--max-plies N] [--sfen S] [--quiet] [--show]
./daishogi batch
```

`batch` reads one command per line:

| Command | Reply |
|---|---|
| `position startpos [moves m1 m2 ...]` | nothing, or `error ...` |
| `position sfen <4 fields> [moves ...]` | nothing, or `error ...` (each move is validated) |
| `legal` | `legal m1 m2 ...` |
| `transitions` | `<usi> <sfen after the move>` for each legal move, then `end` |
| `sfen` | `sfen <current position>` |
| `status` | `status ongoing`, `status win <sente\|gote> <royals\|nomoves\|perpetual>`, or `status draw repetition` |
| `move <usi>` | `ok` or `error ...` |
| `perft <depth>` | `perft <nodes>` |
| `seed <n>` / `go random` | `ok` / `bestmove <usi>` (or `bestmove none`) |
| `display` | diagram |
| `quit` | — |

A position command builds a game history. Repetition therefore applies to
the moves given after `moves`.
