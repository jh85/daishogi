# Two-letter SFEN for large shogi variants

As of 2026-11-21. The normative format reference is [FORMAT.md](FORMAT.md);
this document records the design and the reasons for it.

## Summary

Dai shogi SFEN now writes every piece as exactly two letters, uppercase for Sente and lowercase for Gote (`GO` / `go`). A promoted piece is `+` plus the code of the piece it came from (`+KN`). The old style mixed one letter with primed letters (`G`, `N'`).

The scheme applies to dai shogi and every larger variant. Chu shogi and smaller keep one-letter codes. The engine, the test harness and the docs have switched, and all tests pass on the new format.

## Why two letters

The alphabet has 26 letters, but dai shogi has 29 piece kinds and taikyoku shogi about 209. One-letter codes ran out, and the primed letters that filled the gap (`N'`, `S'`, `I'`, `B'`, …) have no system, read poorly and need shell quoting.

- **Fixed width, not mixed.** With one- and two-letter codes mixed, `ln` could be lance + knight or one piece. Case already marks the side, so it can't also mark where a code ends. A fixed width needs no extra marker.
- **Typos are caught.** With fixed width, a run with an odd number of letters is a syntax error. With mixed codes, a typo can silently parse as another valid position, which matters for test data and counting.
- **Room to grow.** Two letters give 676 codes, well over taikyoku's 209 kinds plus its promoted-only pieces.
- **Cost.** SFEN strings get about twice as long. Test data is gzip-compressed, and state-space counting works on the internal board, not on SFEN text, so neither is affected.
- **Width per variant.** Standard and chu shogi stay one letter, so they remain compatible with lishogi and existing tools. The variant is always known when parsing, so the string doesn't need to say which width it uses.

## Format

An SFEN has four space-separated fields; fields 2–4 may be omitted and default to `b - 1`.

```
<board> <side to move> <lion-capture square> <move number>
```

| Field | Content |
| --- | --- |
| Board | 15 ranks from `a` (Gote's back rank) to `o`, separated by `/`. Each rank lists files 15 → 1. Every rank must add up to exactly 15 squares. |
| Side to move | `b` Sente, `w` Gote |
| Lion-capture square | `-` if off; otherwise the square where a non-lion just took a lion (先獅子). Only whether it is set matters for the rules. |
| Move number | Informational; ignored by the rules and repetition |

A rank is a sequence of tokens:

| Token | Meaning | Example |
| --- | --- | --- |
| Decimal number 1–15 | That many empty squares | `4`, `15` |
| Two uppercase letters | Sente piece | `GO` |
| Two lowercase letters | Gote piece | `go` |
| `+` and two letters | Promoted piece, named by its origin | `+KN` gold from knight, `+dh` Gote horned falcon |

Keeping the origin in the promoted code matters: `+KN` and `+AB` move alike but are distinct pieces, as in the reference. For example, `laknst2KI` reads `la | kn | st | 2 | KI`.

The parser rejects:

- a single letter (`G`) or old primed codes (`N'`): "expected a two-letter piece code";
- mixed case (`Go`): "mixed-case piece code";
- unknown codes (`ZZ`);
- `+` on a piece that cannot promote (`+KI`, `+LI`, `+FK`) or a double `++`;
- wrong rank lengths;
- unpromoted pieces on squares from which they could never move: a pawn, lance, iron or stone general on the last rank, or a knight on the last two ranks.

It does not enforce the reference's piece-count limits per side.

## How codes are chosen

Codes come from the English name, by four rules applied in order. The same piece should get the same code in every large variant.

1. **Two-word names: the initials.** Dragon horse `DH`, Free king `FK`.
2. **One-word names: the first two letters.** Lance `LA`, Lion `LI`.
3. **"X general": the first two letters of X.** Gold `GO`, Iron `IR`. `GG` is left for the great general (大将) of the larger variants.
4. **Clash: the more common piece keeps the code.** The other takes its first letter and the next distinctive consonant: King `KI`, Kirin `KR`; White horse `WH`, Whale `WL`.

"More common" means the piece found in more variants. Every dai shogi piece also appears in tenjiku, dai-dai, maka-dai-dai, tai and taikyoku shogi, so dai shogi pieces keep their codes when those variants are added.

## Dai shogi codes

The 29 kinds, in the reference's PieceId order. Promoted codes are `+` plus the origin code; the haitaka ids are Shogitter's, used by `tests/refconv.py`. The eight golds promoted from knight, iron, stone, evil wolf, angry boar, cat sword, violent ox and flying dragon share id `do` in the reference and are told apart there by origin.

| Code | Piece | 駒 | haitaka id | Old code | Promoted | Promotes to | haitaka id |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `PA` | Pawn | 歩兵 | aa | P | `+PA` | Gold (と金) | ai |
| `LA` | Lance | 香車 | ab | L | `+LA` | White horse 白駒 | cu |
| `KN` | Knight | 桂馬 | ac | N' | `+KN` | Gold | do |
| `SI` | Silver general | 銀将 | ad | S | `+SI` | Vertical mover | de |
| `GO` | Gold general | 金将 | ae | G | `+GO` | Rook | dd |
| `BI` | Bishop | 角行 | af | B | `+BI` | Dragon horse | dl |
| `RO` | Rook | 飛車 | ag | R | `+RO` | Dragon king | dm |
| `KI` | King | 玉将 / 王将 | ah | K | — | — | — |
| `DH` | Dragon horse | 龍馬 | am | H | `+DH` | Horned falcon 角鷹 | cz |
| `DK` | Dragon king | 龍王 | an | D | `+DK` | Soaring eagle 飛鷲 | db |
| `LI` | Lion | 獅子 | bl | N | — | — | — |
| `DE` | Drunk elephant | 醉象 | cf | E | `+DE` | Prince 太子 (royal) | cg |
| `CO` | Copper general | 銅将 | ci | C | `+CO` | Side mover | df |
| `FL` | Ferocious leopard | 猛豹 | cl | F | `+FL` | Bishop | dg |
| `RC` | Reverse chariot | 反車 | cm | A | `+RC` | Whale 鯨鯢 | cv |
| `BT` | Blind tiger | 盲虎 | cn | T | `+BT` | Flying stag 飛鹿 | cw |
| `KR` | Kirin | 麒麟 | co | O | `+KR` | Lion | dh |
| `PH` | Phoenix | 鳳凰 | cp | X | `+PH` | Free king | dj |
| `GB` | Go-between | 仲人 | cq | I | `+GB` | Drunk elephant | dk |
| `SM` | Side mover | 横行 | cr | M | `+SM` | Free boar 奔猪 | cx |
| `VM` | Vertical mover | 竪行 | cs | V | `+VM` | Flying ox 飛牛 | cy |
| `FK` | Free king | 奔王 | ct | Q | — | — | — |
| `IR` | Iron general | 鉄将 | dp | I' | `+IR` | Gold | do |
| `ST` | Stone general | 石将 | dq | S' | `+ST` | Gold | do |
| `EW` | Evil wolf | 悪狼 | dr | W' | `+EW` | Gold | do |
| `AB` | Angry boar | 嗔猪 | ds | B' | `+AB` | Gold | do |
| `CS` | Cat sword | 猫刄 | dt | C' | `+CS` | Gold | do |
| `VO` | Violent ox | 猛牛 | du | O' | `+VO` | Gold | do |
| `FD` | Flying dragon | 飛龍 | dv | D' | `+FD` | Gold | do |

## Reserved codes and the taikyoku check

Eight pieces that appear in dai shogi only as promotions are ordinary pieces in larger variants. Their codes are reserved now so the shared table never has to change. In dai shogi they are still written with `+`.

| Reserved | Piece | 駒 | In dai shogi |
| --- | --- | --- | --- |
| `WH` | White horse | 白駒 | `+LA` |
| `WL` | Whale | 鯨鯢 | `+RC` |
| `HF` | Horned falcon | 角鷹 | `+DH` |
| `SE` | Soaring eagle | 飛鷲 | `+DK` |
| `CP` | Crown prince | 太子 | `+DE` |
| `FS` | Flying stag | 飛鹿 | `+BT` |
| `FB` | Free boar | 奔猪 | `+SM` |
| `FO` | Flying ox | 飛牛 | `+VM` |

A light check against English Wikipedia's taikyoku piece table (301 names, matched by kanji) found no reason to change any dai shogi code. 15 of our 37 codes are codes other taikyoku pieces would also get under the same rules. Since the dai shogi pieces exist in every larger variant, rule 4 lets them keep the codes.

| Code | Ours | Other pieces that would get it |
| --- | --- | --- |
| `FD` | 飛龍 Flying dragon | 火鬼 Fire demon, 奔鬼 Free demon, 奔龍 Free dragon, 猛龍 Fierce dragon, 火龍 Fire dragon, 森鬼 Forest demon, 奔犬 Free dog, 奔獏 Free dream-eater |
| `CS` | 猫刄 Cat sword | 車兵 Chariot soldier, 炮兵 Cannon soldier, 弓兵 Crossbow soldier, 蟠蛇 Coiled serpent, 中旗 Center standard |
| `FS` | 飛鹿 Flying stag | 奔鹿 Free stag, 猛鹿 Fierce stag, 奔蛇 Free serpent, 飛燕 Flying swallow, 前旗 Front standard |
| `FO`, `FB` | 飛牛, 奔猪 | 奔牛 Free ox, 火牛 Fire ox; 奔熊 Free bear, 猛熊 Fierce bear, 奔翅 Free bird |
| `RO`, `LA`, `LI`, `RC`, `SM`, `GB`, … | 飛車, 香車, 獅子, 反車, 横行, 仲人, … | one or two minor pieces each (走牛 Running ox, 左軍 Left army, 左鉄車 Left iron chariot, …) |

Taikyoku fits easily in 676 codes, but the codes stop being guessable: an estimated 193 of the 301 pieces land on one of 65 shared codes, so about 130 would need a fallback. The check used only Wikipedia's main table (207 kinds in its text; Shogitter and haitaka use 209), and three-word names got a rough rule.

## Examples

The start position (`startpos` on the command line):

```
laknstircosigokigosicoirstknla/rc1cs1fl1btdebt1fl1cs1rc/1vo1ab1ewphlikrew1ab1vo1/rofdsmvmbidhdkfkdkdhbivmsmfdro/papapapapapapapapapapapapapapa/4gb5gb4/15/15/15/4GB5GB4/PAPAPAPAPAPAPAPAPAPAPAPAPAPAPA/ROFDSMVMBIDHDKFKDKDHBIVMSMFDRO/1VO1AB1EWKRLIPHEW1AB1VO1/RC1CS1FL1BTDEBT1FL1CS1RC/LAKNSTIRCOSIGOKIGOSICOIRSTKNLA b - 1
```

Gote's back rank, before and after:

```
old: ln's'i'csgkgsci's'n'l
new: laknstircosigokigosicoirstknla
```

The board as `./daishogi show startpos` prints it (top three ranks):

```
      15  14  13  12  11  10   9   8   7   6   5   4   3   2   1
   |  la  kn  st  ir  co  si  go  ki  go  si  co  ir  st  kn  la  | a
   |  rc   .  cs   .  fl   .  bt  de  bt   .  fl   .  cs   .  rc  | b
   |   .  vo   .  ab   .  ew  ph  li  kr  ew   .  ab   .  vo   .  | c
```

A position from the test corpus with promoted pieces (`+PH`, `+BI`, Gote's `+bi`) and the lion-capture restriction set: Gote's lion was just taken on 12c by a non-lion. Sente to move, 169 legal moves.

```
1vost2ki4co2knla/la1dkab6fl2strc/rc1roir9vo1/2fl+PH1+BI3kr1sm1abro/9desm3fd/10ew4/pa3papa2pa2BIpaKN1/2pa8DKPApapa/2PA6SIGB4/1VO+bi1+VM4PAPA2PAPA/PA3PA1PA1GO1VO4/1FLAB1GOEW3dh1pa1FD1/RC2CSCORO8RC/LA1SMSTIRSI1BT2FLKR1ABLA/2FD4KIBT6 b 12c 1
```

A small lion position: Sente's lion on 8h may take the protected lion on 8g because they are adjacent.

```
14ki/15/15/15/15/7go7/7li7/7LI7/15/15/15/15/15/15/KI14 b - 1
```

## Compatibility and verification

Old-style dai shogi SFENs are no longer accepted; convert them with the old-code column of the table above. Moves (`8h7g6f`, `8e8d+`), squares and fields 2–4 are unchanged. Chu shogi and standard shogi SFENs are unaffected.

The switch touched:

- the engine's piece table (`src/types.cpp`) and parser (`src/notation.cpp`);
- the reference converter `tests/refconv.py` and the corpus builder `tests/make_corpus.py`;
- the SFENs in the unit tests, plus 5 new parsing checks;
- [FORMAT.md](FORMAT.md), [RULES.md](RULES.md) and the README.

| Check | Result |
| --- | --- |
| Regenerated corpus vs. old corpus with codes renamed | Identical in all 14,379 records, apart from stored position hashes and case names, which contain the codes |
| `make test` | 732 unit checks; 14,251 positions and 134 games match the reference corpus |
| Mutation check | 19/19 mutants caught |
| Fresh `make reftest SEED=777 GAMES=40` | All OK |
| Perft from the start, depth 3 | 358,191, unchanged |

## Open questions

- [ ] Should taikyoku keep two letters with many arbitrary fallback codes, or use a fixed three-letter width for readability? The per-variant width allows either; three letters would mean a piece's code differs between taikyoku and the smaller variants.
- [ ] Fallback codes for the pieces that lose a clash (e.g. Fire demon, which wants `FD`) are decided when their variant is added, starting with tenjiku.
- [ ] Should the shared code table live in one file used by all large-variant engines, rather than in each engine's piece table?
