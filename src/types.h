// Dai shogi (大将棋, 15x15) core types and the piece table.
//
// Board coordinates (same convention as the chu shogi generator in
// ../chushogi and lishogi's chushogi notation, extended to 15x15):
//   square index = y * 15 + x
//   x = file index   (file 1 -> 0 ... file 15 -> 14); file 1 is on Sente's
//                    right, as in Japanese notation
//   y = rank index   (rank a -> 0 ... rank o -> 14); rank a is Gote's back
//                    rank (the top of the diagram)
// A square is written <file number><rank letter>, e.g. Sente's king starts
// on 8o and Gote's on 8a.
//
// Movement vectors below are written from Sente's point of view:
//   dy = -1 is forward (towards rank a), dx = +1 is towards higher file
//   numbers (Sente's left). Gote's vectors are the point reflection
//   (dx, dy) -> (-dx, -dy), exactly as in the reference implementation.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dai {

enum Color : uint8_t { Sente = 0, Gote = 1 };
inline Color operator~(Color c) { return c == Sente ? Gote : Sente; }

// Piece kinds. The numbering is identical to the PieceId numbering of the
// haitaka-variants reference (DAI_PIECES), which keeps the test-harness
// conversion trivial. 0..28 are the 29 kinds of the initial setup, 29..54 the
// promoted kinds. A promoted kind remembers its origin (e.g. the five kinds
// of "gold from X" move alike but are distinct, as in the reference).
enum Kind : uint8_t {
  Pawn = 0, Lance, Knight, Silver, Gold, Bishop, Rook, King,
  DragonHorse, DragonKing, Lion, DrunkElephant, Copper, Leopard,
  ReverseChariot, BlindTiger, Kirin, Phoenix, GoBetween, SideMover,
  VerticalMover, FreeKing, Iron, Stone, EvilWolf, AngryBoar, CatSword,
  ViolentOx, FlyingDragon,
  // promoted kinds (+ and the origin's code in SFEN)
  Tokin,            // +PA  gold movement
  WhiteHorse,       // +LA
  GoldN,            // +KN  gold from knight
  GoldI,            // +IR  gold from iron general
  GoldSt,           // +ST  gold from stone general
  GoldEW,           // +EW  gold from evil wolf
  GoldAB,           // +AB  gold from angry boar
  GoldCS,           // +CS  gold from cat sword
  GoldVO,           // +VO  gold from violent ox
  GoldFD,           // +FD  gold from flying dragon
  VerticalMoverS,   // +SI  vertical mover from silver
  RookG,            // +GO  rook from gold
  HorseB,           // +BI  dragon horse from bishop
  DragonR,          // +RO  dragon king from rook
  HornedFalcon,     // +DH
  SoaringEagle,     // +DK
  Prince,           // +DE  royal
  SideMoverC,       // +CO  side mover from copper
  BishopF,          // +FL  bishop from ferocious leopard
  Whale,            // +RC
  FlyingStag,       // +BT
  LionO,            // +KR  lion from kirin
  FreeKingX,        // +PH  free king from phoenix
  ElephantI,        // +GB  drunk elephant from go-between
  FreeBoar,         // +SM
  FlyingOx,         // +VM
  KindCount
};

constexpr int kFiles = 15;
constexpr int kRanks = 15;
constexpr int kSquares = kFiles * kRanks;
constexpr int kNoSquare = -1;
constexpr int kPromotionRanks = 5;

inline int sqX(int sq) { return sq % kFiles; }
inline int sqY(int sq) { return sq / kFiles; }
inline int makeSq(int x, int y) { return y * kFiles + x; }
inline bool onBoard(int x, int y) {
  return 0 <= x && x < kFiles && 0 <= y && y < kRanks;
}
// Chebyshev distance.
inline int dist(int a, int b) {
  int dx = sqX(a) - sqX(b); if (dx < 0) dx = -dx;
  int dy = sqY(a) - sqY(b); if (dy < 0) dy = -dy;
  return dx > dy ? dx : dy;
}
// Rank counted from the given side's far end: 0 = the opponent's back rank.
inline int relRank(int sq, Color c) {
  return c == Sente ? sqY(sq) : kRanks - 1 - sqY(sq);
}

// Piece id stored on the board: 0 = empty, otherwise (kind << 1 | color) + 1.
using Piece = uint8_t;
inline Piece makePiece(Kind k, Color c) {
  return static_cast<Piece>(((static_cast<int>(k) << 1) | c) + 1);
}
inline Kind kindOf(Piece p) { return static_cast<Kind>((p - 1) >> 1); }
inline Color colorOf(Piece p) { return static_cast<Color>((p - 1) & 1); }

struct Dir { int8_t dx, dy; };
// A sliding direction; limit 0 = unlimited, otherwise the maximum number of
// squares (violent ox and flying dragon: 2, blocked by intervening pieces).
struct Ray { int8_t dx, dy, limit; };

struct KindInfo {
  std::string code;         // SFEN code without color: two letters, "+" for promoted, e.g. "KN", "+DH"
  std::string name;         // English name
  std::string kanji;        // Japanese name
  std::string refId;        // haitaka-variants piece id (e.g. "aa")
  std::vector<Dir> steps;   // single steps and jumps (never blocked)
  std::vector<Ray> rays;    // sliding moves
  int promotesTo = -1;      // promoted kind, or -1
  int base = -1;            // for promoted kinds: the unpromoted kind
  bool royal = false;       // king and prince
  bool lion = false;        // lion and lion-from-kirin (lion capture rules)
  int deadRanks = 0;        // unpromoted piece may not end on these ranks
  int fileLimit = 0;        // max same-kind own pieces per file (0 = none)
};

const std::array<KindInfo, KindCount>& kindTable();
inline const KindInfo& info(Kind k) { return kindTable()[k]; }

// Lion-power continuations: for a piece of kind k that has just made the
// single step `first` (Sente POV), the directions in which it may take a
// second single step. nullptr if the step does not allow a continuation.
const std::vector<Dir>* continuation(Kind k, Dir first);

// A move: a single move from -> to, or a two-step (lion-power) move
// from -> mid -> to. `to` may equal `from` (igui / passing).
struct Move {
  int16_t from = kNoSquare;
  int16_t mid = kNoSquare;
  int16_t to = kNoSquare;
  bool promote = false;
  bool operator==(const Move& o) const {
    return from == o.from && mid == o.mid && to == o.to && promote == o.promote;
  }
  bool operator<(const Move& o) const {
    if (from != o.from) return from < o.from;
    if (mid != o.mid) return mid < o.mid;
    if (to != o.to) return to < o.to;
    return promote < o.promote;
  }
};

}  // namespace dai
