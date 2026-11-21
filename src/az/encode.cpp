// Ported from ../chushogi (src/az/encode.cpp, commit d578c81).

#include "encode.h"

#include <algorithm>

namespace az {

namespace {

// Fixed unit-direction table for the double-move block.
constexpr dai::Dir kDirs[8] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0},
                               {1, 0},   {-1, 1}, {0, 1},  {1, 1}};

int dirIndex(int dx, int dy) {
  for (int i = 0; i < 8; ++i)
    if (kDirs[i].dx == dx && kDirs[i].dy == dy) return i;
  return -1;
}

}  // namespace

int moveToIndex(const dai::Move& m) {
  if (m.mid == dai::kNoSquare) {
    const int idx = m.from * kBoard + m.to;
    return m.promote ? kPolicyMove + idx : idx;
  }
  const int md = dirIndex(dai::sqX(m.mid) - dai::sqX(m.from),
                          dai::sqY(m.mid) - dai::sqY(m.from));
  const int td = dirIndex(dai::sqX(m.to) - dai::sqX(m.mid),
                          dai::sqY(m.to) - dai::sqY(m.mid));
  return 2 * kPolicyMove + m.from * 64 + md * 8 + td;
}

dai::Move indexToMove(int idx) {
  dai::Move m;
  if (idx < 2 * kPolicyMove) {
    m.promote = idx >= kPolicyMove;
    const int i = idx % kPolicyMove;
    m.from = static_cast<int16_t>(i / kBoard);
    m.to = static_cast<int16_t>(i % kBoard);
    return m;
  }
  const int i = idx - 2 * kPolicyMove;
  m.from = static_cast<int16_t>(i / 64);
  const int md = (i / 8) % 8, td = i % 8;
  const int mx = dai::sqX(m.from) + kDirs[md].dx, my = dai::sqY(m.from) + kDirs[md].dy;
  m.mid = static_cast<int16_t>(dai::makeSq(mx, my));
  m.to = static_cast<int16_t>(dai::makeSq(mx + kDirs[td].dx, my + kDirs[td].dy));
  return m;
}

void encodePlanes(const dai::Position& pos, int repCount, int ply, float* out) {
  std::fill(out, out + kNumPlanes * kBoard, 0.0f);
  for (int s = 0; s < kBoard; ++s) {
    const dai::Piece p = pos.board[s];
    if (!p) continue;
    const int kind = static_cast<int>(dai::kindOf(p));
    const bool ours = dai::colorOf(p) == pos.sideToMove;
    out[(ours ? kind : kKindPlanes + kind) * kBoard + s] = 1.0f;
  }
  if (pos.lionCapture != dai::kNoSquare)
    out[kLionCapturePlane * kBoard + pos.lionCapture] = 1.0f;
  if (repCount >= 1)
    std::fill(out + kRepeatOncePlane * kBoard, out + (kRepeatOncePlane + 1) * kBoard, 1.0f);
  if (repCount >= 2)
    std::fill(out + kRepeatTwicePlane * kBoard, out + (kRepeatTwicePlane + 1) * kBoard, 1.0f);
  const float progress = std::min(static_cast<float>(ply) / kPlyCap, 1.0f);
  std::fill(out + kProgressPlane * kBoard, out + kNumPlanes * kBoard, progress);
}

void encodePosition(const dai::Position& pos, int repCount, int ply, float* out) {
  if (pos.sideToMove == dai::Gote)
    encodePlanes(pos.flipped(), repCount, ply, out);
  else
    encodePlanes(pos, repCount, ply, out);
}

float packPlanes(const float* planes, uint8_t* out) {
  std::fill(out, out + kPackedBytes, 0);
  for (int bit = 0; bit < kBinaryPlanes * kBoard; ++bit)
    if (planes[bit] != 0.f) out[bit / 8] |= static_cast<uint8_t>(0x80 >> (bit % 8));
  return planes[kProgressPlane * kBoard];
}

}  // namespace az
