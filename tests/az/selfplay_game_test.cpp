// Self-play game loop with a scripted evaluator: the go-betweens shuffle
// 11j11i / 11i11j (canonical frame, so both sides play the same shape) and
// the start position recurs every four plies. The game must end by
// fourfold repetition at ply 12 as a draw, and every recorded sample must
// carry the real-frame repetition count, the right side to move, z and the
// planes of the independently replayed position.

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../src/az/selfplay.h"
#include "../../src/notation.h"

namespace {

void check(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}

dai::Move usiMove(const char* s) {
  dai::Move m;
  check(dai::parseUsi(s, m), std::string("move ") + s);
  return m;
}

struct CycleEval : az::Evaluator {
  void evaluate(int n, const float* x, float* p, float* v) override {
    for (int i = 0; i < n; ++i) {
      const int ply = static_cast<int>(
          std::lround(x[(i * az::kNumPlanes + az::kProgressPlane) * az::kBoard] * az::kPlyCap));
      const dai::Move move = usiMove(ply % 4 < 2 ? "11j11i" : "11i11j");
      std::fill(p + i * az::kPolicySize, p + (i + 1) * az::kPolicySize, -100.f);
      p[i * az::kPolicySize + az::moveToIndex(move)] = 100.f;
      v[3 * i] = 0.f;
      v[3 * i + 1] = -100.f;
      v[3 * i + 2] = 0.f;
    }
  }
};

}  // namespace

int main() {
  try {
    az::SelfplayConfig cfg;
    cfg.sims = cfg.mcts.sims = 8;
    cfg.batchSize = cfg.mcts.batchSize = 4;
    cfg.tempPlies = 0;
    cfg.mcts.rootNoise = false;
    CycleEval eval;
    const az::GameRecord rec = az::playGame(dai::kStartSfen, cfg, eval);
    check(rec.reason == "repetition" && rec.result == 0,
          "fourfold repetition draw expected, got " + rec.reason + " " +
              std::to_string(rec.result));
    check(rec.moves.size() == 12, "game length " + std::to_string(rec.moves.size()));
    const char* real[4] = {"11j11i", "5f5g", "11i11j", "5g5f"};
    for (size_t i = 0; i < rec.moves.size(); ++i)
      check(rec.moves[i] == real[i % 4], "move " + std::to_string(i) + " " + rec.moves[i]);
    check(rec.samples.size() == 12, "one sample per move");

    dai::Position pos;
    check(dai::parseSfen(dai::kStartSfen, pos), "start");
    std::vector<float> planes(az::kNumPlanes * az::kBoard);
    std::vector<uint8_t> packed(az::kPackedBytes);
    for (int ply = 0; ply < 12; ++ply) {
      const az::Sample& s = rec.samples[ply];
      check(s.ply == ply && s.stm == ply % 2, "sample ply / side to move");
      check(s.z == .5f, "draw target");
      const int reps = ply / 4;  // earlier occurrences of this position
      az::encodePosition(pos, reps, ply, planes.data());
      const float progress = az::packPlanes(planes.data(), packed.data());
      check(packed == s.packedPlanes && progress == s.progress,
            "planes at ply " + std::to_string(ply));
      check(planes[az::kRepeatOncePlane * az::kBoard] == (reps >= 1) &&
                planes[az::kRepeatTwicePlane * az::kBoard] == (reps >= 2),
            "repetition planes at ply " + std::to_string(ply));
      float sum = 0;
      for (const auto& kv : s.policy) sum += kv.second;
      check(std::abs(sum - 1.f) < 1e-5, "policy sums to one");
      pos.apply(usiMove(real[ply % 4]));
    }
  } catch (const std::exception& e) {
    std::cout << "FAIL: " << e.what() << "\n";
    return 1;
  }
  std::cout << "OK: scripted self-play game ends by fourfold repetition; samples carry "
               "real-frame repetition counts, side to move, z and replayed planes\n";
  return 0;
}
