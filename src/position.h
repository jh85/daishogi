// Dai shogi position, move application and attack helpers.

#pragma once

#include <array>
#include <cstdint>

#include "types.h"

namespace dai {

struct Position {
  std::array<Piece, kSquares> board{};  // 0 = empty
  Color sideToMove = Sente;
  // Lion-capture state (先獅子): if the last hop of the previous move was a
  // non-lion piece capturing a lion, the square of that capture; otherwise
  // kNoSquare. While it is set, the side to move may not capture a lion with
  // a non-lion piece. Serialized as the third SFEN field.
  int16_t lionCapture = kNoSquare;
  int moveNumber = 1;  // fourth SFEN field; not part of the game state

  Piece at(int sq) const { return board[sq]; }

  // Applies a move (assumed legal). Each hop updates lionCapture, so after a
  // two-step move the state reflects the second hop (reference behavior).
  void apply(const Move& m);

  int royals(Color c) const;
  // Zobrist key of everything that defines the game state: board, side to
  // move and whether the lion-capture restriction is active.
  uint64_t key() const;

  // The same position seen from the other side: the board rotated 180
  // degrees and the colors swapped (see flipSquare below). Used to present
  // every position to a neural network from the side to move's view.
  Position flipped() const;

  // Same game state (moveNumber and the exact lion-capture square ignored).
  bool sameState(const Position& o) const {
    return board == o.board && sideToMove == o.sideToMove &&
           (lionCapture != kNoSquare) == (o.lionCapture != kNoSquare);
  }
};

// ---- Side-to-move canonicalization ----
// Gote's movement vectors are the point reflection of Sente's, so rotating
// the board 180 degrees and swapping the colors maps every position to an
// equivalent one with the other side to move: legal moves, promotion zones,
// the per-file limits, the lion rules and game results all correspond. The
// initial setup is invariant under this transform (unlike under a mirror of
// the ranks only, which would swap the kirin and phoenix files).
inline int flipSquare(int sq) { return kSquares - 1 - sq; }
inline Piece flipPiece(Piece p) {
  return p ? static_cast<Piece>(((p - 1) ^ 1) + 1) : Piece{0};
}
inline Move flipMove(const Move& m) {
  Move r = m;
  r.from = static_cast<int16_t>(flipSquare(m.from));
  r.to = static_cast<int16_t>(flipSquare(m.to));
  if (m.mid != kNoSquare) r.mid = static_cast<int16_t>(flipSquare(m.mid));
  return r;
}

inline Dir physical(Dir d, Color c) {
  return c == Sente ? d : Dir{static_cast<int8_t>(-d.dx),
                              static_cast<int8_t>(-d.dy)};
}

// Geometric attack test: can the piece on `from` (of its own color) reach
// `to` in one step/jump/slide, ignoring what stands on `to`? Square `vacant`
// (kNoSquare allowed) is treated as empty for blocking purposes. Lion-power
// second steps are not included: every square they reach is also one of the
// piece's direct jump targets.
bool pieceAttacks(const Position& pos, int from, int to, int vacant);

// Is `target` attacked by any piece of color `by` (pieces on `vacant` ignored
// and treated as empty)?
bool attackedBy(const Position& pos, Color by, int target, int vacant);

// Is any royal (king or prince) of `c` attacked? Used by the perpetual-check
// rule of the repetition adjudication only (dai shogi has no check rule).
bool royalAttacked(const Position& pos, Color c);

}  // namespace dai
