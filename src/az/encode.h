// AlphaZero input encoding for dai shogi.
//
// Ported from ../chushogi (src/az/encode.h, commit d578c81), adapted to the
// 15x15 board and the 55 dai shogi piece kinds.
//
// Positions are canonicalized to the side-to-move's perspective first
// (Position::flipped() when Gote is to move: 180-degree rotation + color
// swap), then encoded as 114 planes of 15x15:
//
//   planes   0..54  : our pieces, one plane per Kind (src/types.h order)
//   planes  55..109 : their pieces
//   plane  110      : one-hot on the lion-capture square (先獅子 state)
//   plane  111      : broadcast 1 if the position occurred once before
//   plane  112      : broadcast 1 if it occurred twice or more before
//   plane  113      : broadcast ply / kPlyCap (draw-cap progress)
//
// Policy space (115,650 logits): the from x to matrix (225 x 225 = 50,625),
// a second from x to matrix for promotions, and a from x midDir(8) x
// toDir(8) block (225 x 64 = 14,400) for two-step lion-power moves (lion,
// lion from kirin, horned falcon, soaring eagle; incl. igui and passes).
// Legal-move masking and softmax happen outside the net.

#pragma once

#include <cstdint>
#include <vector>

#include "../position.h"

namespace az {

constexpr int kBoard = dai::kSquares;                          // 225
constexpr int kPolicyMove = kBoard * kBoard;                   // 50,625
constexpr int kPolicyPromote = kPolicyMove;                    // 2nd from-to matrix
constexpr int kPolicyDouble = kBoard * 8 * 8;                  // 14,400
constexpr int kPolicySize = 2 * kPolicyMove + kPolicyDouble;   // 115,650

constexpr int kKindPlanes = dai::KindCount;                    // 55
constexpr int kLionCapturePlane = 2 * kKindPlanes;             // 110
constexpr int kRepeatOncePlane = kLionCapturePlane + 1;        // 111
constexpr int kRepeatTwicePlane = kLionCapturePlane + 2;       // 112
constexpr int kProgressPlane = kLionCapturePlane + 3;          // 113
constexpr int kNumPlanes = kProgressPlane + 1;                 // 114
constexpr int kBinaryPlanes = kProgressPlane;                  // 0..112

// Game length cap (plies from the game start): reaching it is a draw. The
// progress plane is ply / kPlyCap.
constexpr int kPlyCap = 1500;

// Move <-> policy index. Moves must be in the canonical (flipped) frame.
int moveToIndex(const dai::Move& m);
dai::Move indexToMove(int idx);
inline bool isDoubleIndex(int idx) { return idx >= 2 * kPolicyMove; }

// Encodes `pos` (already canonicalized by the caller) into `out`
// (kNumPlanes * kBoard floats, plane-major). repCount = how many times this
// position occurred earlier in the game (0/1/2+), ply = plies played.
void encodePlanes(const dai::Position& pos, int repCount, int ply, float* out);

// Convenience: canonicalize + encode.
void encodePosition(const dai::Position& pos, int repCount, int ply, float* out);

// Packs the binary planes 0..112 MSB-first into out (113*225 bits, padded
// to whole bytes: 3,179 bytes); the progress plane is returned separately.
constexpr int kPackedBytes = (kBinaryPlanes * kBoard + 7) / 8;
float packPlanes(const float* planes, uint8_t* out);

}  // namespace az
