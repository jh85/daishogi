// MCTS value/perspective tests. Ported from ../chushogi
// (tests/az_search_value_test.cpp, commit d578c81) with dai shogi fixtures:
// the forced pawn move 1k1j stands for chu's 12i12h, the go-between
// shuffle 11j11i for 9h9g, and the rook-takes-king position for the chu one.

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#include "../../src/az/mcts.h"
#include "../../src/notation.h"

namespace {

dai::Move usiMove(const char* s) {
  dai::Move m;
  if (!dai::parseUsi(s, m)) throw std::runtime_error(std::string("move ") + s);
  return m;
}

float progressOf(const std::vector<float>& x) { return x[az::kProgressPlane * az::kBoard]; }

struct Eval : az::Evaluator {
  std::vector<std::vector<float>> inputs;
  void evaluate(int n, const float* x, float* p, float* v) override {
    const dai::Move move = usiMove("1k1j");
    for (int i = 0; i < n; ++i) {
      inputs.emplace_back(x + i * az::kNumPlanes * az::kBoard,
                          x + (i + 1) * az::kNumPlanes * az::kBoard);
      std::fill(p + i * az::kPolicySize, p + (i + 1) * az::kPolicySize, -100.f);
      p[i * az::kPolicySize + az::moveToIndex(move)] = 100.f;
      v[3 * i] = std::log(.8f);
      v[3 * i + 1] = -100.f;
      v[3 * i + 2] = std::log(.2f);
    }
  }
};

struct CaptureEval : az::Evaluator {
  void evaluate(int n, const float*, float* p, float* v) override {
    const dai::Move capture = usiMove("8m8c");
    for (int i = 0; i < n; ++i) {
      std::fill(p + i * az::kPolicySize, p + (i + 1) * az::kPolicySize, -1000.f);
      p[i * az::kPolicySize + az::moveToIndex(capture)] = 1000.f;
      v[3 * i] = 0;
      v[3 * i + 1] = -100.f;
      v[3 * i + 2] = 0;
    }
  }
};

// Go-between forward on plies 0,1 (mod 4) and back on 2,3: the start
// position recurs every four plies.
struct CycleEval : az::Evaluator {
  std::vector<std::vector<float>> inputs;
  void evaluate(int n, const float* x, float* p, float* v) override {
    for (int i = 0; i < n; ++i) {
      inputs.emplace_back(x + i * az::kNumPlanes * az::kBoard,
                          x + (i + 1) * az::kNumPlanes * az::kBoard);
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

struct TemperatureEval : az::Evaluator {
  bool forceRoot = false;
  std::vector<std::vector<float>> inputs;
  void evaluate(int n, const float* x, float* p, float* v) override {
    const dai::Move a = usiMove("1k1j"), b = usiMove("2k2j");
    for (int i = 0; i < n; ++i) {
      inputs.emplace_back(x + i * az::kNumPlanes * az::kBoard,
                          x + (i + 1) * az::kNumPlanes * az::kBoard);
      std::fill(p + i * az::kPolicySize, p + (i + 1) * az::kPolicySize, -1000.f);
      const bool root = x[(i * az::kNumPlanes + az::kProgressPlane) * az::kBoard] == 0.f;
      p[i * az::kPolicySize + az::moveToIndex(a)] = forceRoot && root ? 100.f : std::log(4.f);
      p[i * az::kPolicySize + az::moveToIndex(b)] = forceRoot && root ? -1000.f : 0.f;
      v[3 * i] = std::log(.8f);
      v[3 * i + 1] = -100.f;
      v[3 * i + 2] = std::log(.2f);
    }
  }
};

void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

}  // namespace

int main() {
  try {
    dai::Position root;
    check(dai::parseSfen(dai::kStartSfen, root), "sfen");
    az::MctsConfig cfg;
    cfg.sims = 1;
    cfg.batchSize = 1;
    cfg.rootNoise = false;
    std::mt19937_64 rng(1);
    Eval eval;
    auto one = az::Mcts(cfg).search(root, false, {}, 0, eval, rng);
    check(std::abs(one.rootQ - .2f) < 1e-5, "neural leaf value must be inverted and backed up");
    check(std::abs(one.rootWdl[0] - .8f) < 1e-5, "root diagnostic WDL");
    check(std::abs(one.a0gb - .2f) < 1e-5, "A0GB must enter the fresh one-ply leaf and flip its value");

    dai::Position captureRoot;
    check(dai::parseSfen("15/6go8/7ki7/15/15/15/15/15/15/15/15/15/7RO7/15/KI14 b - 1", captureRoot),
          "capture sfen");
    CaptureEval captureEval;
    auto captured = az::Mcts(cfg).search(captureRoot, false, {}, 0, captureEval, rng);
    check(std::abs(captured.a0gb - 1.f) < 1e-5,
          "A0GB must enter a terminal child and return the proven win");

    cfg.sims = 2;
    Eval deeper;
    auto two = az::Mcts(cfg).search(root, false, {}, 0, deeper, rng);
    check(deeper.inputs.size() == 3, "root plus two evaluated leaves expected");
    dai::Position expected = root;
    for (int i = 0; i < 3; ++i) {
      std::vector<float> planes(az::kNumPlanes * az::kBoard);
      az::encodePosition(expected, 0, i, planes.data());
      check(planes == deeper.inputs[i],
            "search leaves must use canonical encoding and canonical policy indices");
      // Gote's reply is the same canonical move: 1k1j flipped is 15e15f.
      if (i < 2) expected.apply(usiMove(i == 0 ? "1k1j" : "15e15f"));
    }
    check(std::abs(two.rootQ - .5f) < 1e-5, "two-ply value perspective");
    check(std::abs(two.a0gb - .8f) < 1e-5, "A0GB two-ply leaf perspective");

    cfg.sims = 32;
    cfg.batchSize = 8;
    Eval batched;
    auto many = az::Mcts(cfg).search(root, false, {}, 0, batched, rng);
    bool deep = false;
    for (const auto& x : batched.inputs)
      if (progressOf(x) > 1.5f / az::kPlyCap) deep = true;
    check(deep, "batched search must expand beyond evaluated leaves");
    check(many.rootVisits == 32, "visit count");

    // One prior occurrence must cut off in early mode, but fourfold mode
    // must evaluate the leaf until three prior occurrences exist.
    dai::Position child = root;
    child.apply(usiMove("1k1j"));
    const uint64_t childKey = az::gameKey(child);
    cfg.sims = 1;
    cfg.batchSize = 1;
    cfg.collectStats = true;
    for (int prior = 1; prior <= 3; ++prior) {
      const std::vector<uint64_t> history(prior, childKey);
      cfg.repetitionOccurrences = 2;
      Eval early;
      auto e = az::Mcts(cfg).search(root, false, history, 0, early, rng);
      check(std::abs(e.rootQ - .5f) < 1e-5, "early repetition cutoff");
      cfg.repetitionOccurrences = 4;
      Eval exact;
      auto four = az::Mcts(cfg).search(root, false, history, 0, exact, rng);
      check(std::abs(four.a0gb - (prior == 3 ? .5f : .2f)) < 1e-5,
            "A0GB must use terminal draw or nonterminal leaf value");
      check(std::abs(four.rootQ - (prior == 3 ? .5f : .2f)) < 1e-5,
            "fourfold requires three prior occurrences");
      if (prior < 3) {
        check(exact.inputs.size() == 2, "nonterminal repeated leaf must be evaluated");
        check(exact.inputs[1][az::kRepeatOncePlane * az::kBoard] == 1.f,
              "first repetition input plane");
        check(exact.inputs[1][az::kRepeatTwicePlane * az::kBoard] == (prior == 2 ? 1.f : 0.f),
              "second repetition input plane");
      }
      int repetitionVisits = 0;
      for (const auto& move : four.rootMoves) repetitionVisits += move.repetitionVisits;
      check(repetitionVisits == (prior == 3 ? 1 : 0), "diagnostic repetition visits");
    }

    // The same history seen from the other side: a flipped root must use
    // real-frame keys, so the repetition is still found.
    {
      cfg.repetitionOccurrences = 2;
      const dai::Position gote = root.flipped();  // Gote to move, same board
      dai::Position goteChild = gote;
      goteChild.apply(usiMove("15e15f"));
      const std::vector<uint64_t> history(1, az::gameKey(goteChild));
      Eval flipped;
      auto f = az::Mcts(cfg).search(gote.flipped(), true, history, 0, flipped, rng);
      check(std::abs(f.rootQ - .5f) < 1e-5, "repetition with a flipped root");
      // ...and a position with the other side to move is not a repetition,
      // although its canonical view is identical (a lion pass in the
      // symmetric start position).
      const std::vector<uint64_t> twin(1, az::gameKey(child.flipped()));
      Eval notRepeated;
      auto t = az::Mcts(cfg).search(root, false, twin, 0, notRepeated, rng);
      check(std::abs(t.rootQ - .2f) < 1e-5, "flipped twin must not count as a repetition");
    }

    cfg.sims = 12;
    cfg.repetitionOccurrences = 4;
    CycleEval cycle;
    auto cyclic = az::Mcts(cfg).search(root, false, {}, 0, cycle, rng);
    check(cycle.inputs.size() == 12, "fourfold search must include the current root in path history");
    check(cycle.inputs[4][az::kRepeatOncePlane * az::kBoard] == 1.f,
          "root's second occurrence reaches network");
    check(cycle.inputs[8][az::kRepeatTwicePlane * az::kBoard] == 1.f,
          "root's third occurrence reaches network");
    int repetitions = 0;
    for (const auto& move : cyclic.rootMoves) repetitions += move.repetitionVisits;
    check(repetitions == 1, "fourth root occurrence must terminate at depth twelve");

    // Root priors must be softmax(logits/T), with unchanged NN values.
    cfg = az::MctsConfig{};
    cfg.sims = 1;
    cfg.batchSize = 1;
    cfg.rootNoise = false;
    cfg.collectStats = true;
    for (float temperature : {1.f, 1.25f}) {
      cfg.rootPolicyTemperature = temperature;
      TemperatureEval evaluator;
      auto result = az::Mcts(cfg).search(root, false, {}, 0, evaluator, rng);
      float pa = 0, pb = 0, sum = 0;
      for (const auto& edge : result.rootMoves) {
        sum += edge.prior;
        if (dai::moveToUsi(edge.move) == "1k1j") pa = edge.prior;
        if (dai::moveToUsi(edge.move) == "2k2j") pb = edge.prior;
      }
      check(std::abs(pa / pb - std::pow(4.f, 1.f / temperature)) < 1e-4, "root temperature prior ratio");
      check(std::abs(sum - 1.f) < 1e-5, "root priors sum to one");
      check(std::abs(result.rootWdl[0] - .8f) < 1e-5, "temperature must preserve neural WDL");
    }
    // Identical forced root priors isolate deeper-node behavior: changing T
    // must leave the complete evaluated-position sequence unchanged.
    cfg.sims = 64;
    TemperatureEval cold, warm;
    cold.forceRoot = warm.forceRoot = true;
    cfg.rootPolicyTemperature = 1.f;
    std::mt19937_64 coldRng(42), warmRng(42);
    auto coldResult = az::Mcts(cfg).search(root, false, {}, 0, cold, coldRng);
    cfg.rootPolicyTemperature = 1.25f;
    auto warmResult = az::Mcts(cfg).search(root, false, {}, 0, warm, warmRng);
    check(cold.inputs == warm.inputs && coldResult.rootQ == warmResult.rootQ,
          "temperature must affect only root priors");
    for (float invalid : {0.f, -1.f, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()}) {
      cfg.rootPolicyTemperature = invalid;
      bool rejected = false;
      try {
        az::Mcts search(cfg);
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      check(rejected, "invalid root temperatures must be rejected");
    }
  } catch (const std::exception& e) {
    std::cout << "FAIL: " << e.what() << "\n";
    return 1;
  }
  std::cout << "OK: neural backup, canonical inputs/policy, batched expansion, fourfold history, "
               "real-frame repetition keys, repetition planes and root temperature checks passed\n";
  return 0;
}
