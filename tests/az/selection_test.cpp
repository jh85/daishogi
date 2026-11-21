// Self-play move selection and the root royal filter. Ported from
// ../chushogi (tests/az_sampling_test.cpp and the parts of
// tests/az_root_safety_test.cpp that do not need the mate solver, commit
// d578c81), with dai shogi fixtures.

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#include "../../src/az/selfplay.h"
#include "../../src/mate.h"
#include "../../src/movegen.h"
#include "../../src/notation.h"

namespace {

void check(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}

dai::Move move(const char* s) {
  dai::Move m;
  check(dai::parseUsi(s, m), std::string("move ") + s);
  return m;
}

dai::Position position(const char* s) {
  dai::Position p;
  check(dai::parseSfen(s, p), std::string("SFEN ") + s);
  return p;
}

// Every node's policy favours one move (in each node's canonical frame) by
// `margin` logits.
struct ForcedEval : az::Evaluator {
  int index;
  float margin;
  explicit ForcedEval(const char* s, float margin = 2000.f)
      : index(az::moveToIndex(move(s))), margin(margin) {}
  void evaluate(int n, const float*, float* p, float* v) override {
    std::fill(p, p + size_t(n) * az::kPolicySize, -margin / 2);
    for (int i = 0; i < n; ++i) {
      p[size_t(i) * az::kPolicySize + index] = margin / 2;
      v[3 * i] = v[3 * i + 1] = v[3 * i + 2] = 0;
    }
  }
};

void testSampling() {
  const dai::Move best = move("1k1j"), other = move("2k2j");
  az::SearchResult result;
  result.bestMove = best;
  result.policy = {{az::moveToIndex(best), .75f}, {az::moveToIndex(other), .25f}};
  const auto original = result.policy;
  // (samplingPlies, ply): inside the opening window, or -1 = sample throughout.
  for (auto condition : {std::pair<int, int>{20, 19}, {-1, 20}, {-1, 999}}) {
    std::mt19937_64 rng(123);
    int alternatives = 0;
    for (int i = 0; i < 10000; ++i)
      alternatives += dai::moveToUsi(az::selectSelfplayMove(result, condition.first,
                                                            condition.second, rng)) == "2k2j";
    check(std::abs(alternatives / 10000.f - .25f) < .02f,
          "sampling must remain proportional to visits, including late game");
  }
  for (auto condition : {std::pair<int, int>{20, 20}, {20, 999}, {0, 0}}) {
    std::mt19937_64 rng(17), copy = rng;
    for (int i = 0; i < 100; ++i)
      check(dai::moveToUsi(az::selectSelfplayMove(result, condition.first, condition.second,
                                                  rng)) == "1k1j",
            "cutoff must select best move");
    check(rng == copy, "greedy selection must not consume randomness");
  }
  check(result.policy == original, "sampling must not alter the policy training target");
  result.policy = {{az::moveToIndex(best), 1.f}};
  std::mt19937_64 rng(123);
  for (int i = 0; i < 100; ++i)
    check(dai::moveToUsi(az::selectSelfplayMove(result, -1, 999, rng)) == "1k1j",
          "one-hot policy must remain forced");
}

// Gote: king 15a, rook 7a. Sente: king 8o alone. 8o7o and 8o7n walk into
// the rook's file.
const char* kHang = "ki7ro6/15/15/15/15/15/15/15/15/15/15/15/15/15/7KI7 b - 1";

void testHangsLastRoyal() {
  const auto p = position(kHang);
  int hangs = 0;
  for (const auto& m : dai::generateLegal(p)) hangs += az::hangsLastRoyal(p, m);
  check(hangs == 2, "two king moves onto the rook's file, got " + std::to_string(hangs));
  check(az::hangsLastRoyal(p, move("8o7o")), "8o7o hangs");
  check(!az::hangsLastRoyal(p, move("8o9o")), "8o9o is safe");

  // A crown prince (+DE) elsewhere: losing the king is not losing the last royal.
  const auto prince = position("ki7ro6/15/15/15/15/15/15/15/15/15/15/15/15/15/+DE6KI7 b - 1");
  check(!az::hangsLastRoyal(prince, move("8o7o")), "prince survives");

  // No Sente royal at all (a tsume-style attacker).
  const auto kingless = position("ki7ro6/15/15/15/15/15/15/15/15/15/15/15/15/15/7GO7 b - 1");
  check(!az::hangsLastRoyal(kingless, move("8o7o")), "kingless side cannot hang a royal");

  // Capturing the opponent's last royal ends the game first.
  const auto winning =
      position("15/6go8/7ki7/15/15/15/15/15/15/15/15/15/7RO7/15/KI14 b - 1");
  check(!az::hangsLastRoyal(winning, move("8m8c")), "royal capture is not a hang");

  // One lion reply can take king (mid) and prince (to): king 8h, prince
  // 8i, Gote lion 8g. Any quiet Sente move hangs both royals.
  const auto two =
      position("ki14/15/15/15/15/15/7li7/7KI7/7+DE7/15/15/15/1GO13/15/15 b - 1");
  check(az::hangsLastRoyal(two, move("14m14l")),
        "one lion reply can capture king and prince via mid and to");
  check(!az::hangsLastRoyal(two, move("8h8g")), "taking the lion is safe");
}

float edgeQ(const az::SearchResult& r, const char* usi) {
  for (const auto& m : r.rootMoves)
    if (dai::moveToUsi(m.move) == usi) return m.q;
  throw std::runtime_error(std::string("no root move ") + usi);
}

// Positions where the side to move can take the opponent's last royal are
// proven wins inside the tree, so a hanging move is refuted after one visit
// even when the prior insists on it and gives the capture no mass.
void testWinInOne() {
  const auto p = position(kHang);
  az::MctsConfig cfg;
  cfg.sims = 64;
  cfg.batchSize = 8;
  cfg.rootNoise = false;
  cfg.collectStats = true;
  std::mt19937_64 rng(5);
  az::RandomEval uniform;
  auto u = az::Mcts(cfg).search(p, false, {}, 0, uniform, rng);
  check(!az::hangsLastRoyal(p, u.bestMove), "uniform prior: hanging move chosen");
  check(edgeQ(u, "8o7o") == 0.f && edgeQ(u, "8o7n") == 0.f, "hanging moves are proven losses");
  // A prior that prefers the hanging move (65%) and gives the capture
  // nothing: refuted after one visit, then the search moves on.
  ForcedEval leaning("8o7o", 2.f);
  auto l = az::Mcts(cfg).search(p, false, {}, 0, leaning, rng);
  check(dai::moveToUsi(l.bestMove) != "8o7o" && edgeQ(l, "8o7o") == 0.f,
        "leaning prior: the hanging move must be refuted");
  for (int i = 0; i < 200; ++i)
    check(!az::hangsLastRoyal(p, az::selectSelfplayMove(l, -1, 50, rng)),
          "sampling never plays a hanging move here");
  // A prior of exactly 1 on the blunder keeps PUCT on it, but a proven
  // loss is neither chosen nor kept in the policy target. (The other
  // hanging move is never visited, so it is not proven: that case is the
  // root royal filter's.)
  ForcedEval forced("8o7o");
  auto f = az::Mcts(cfg).search(p, false, {}, 0, forced, rng);
  check(edgeQ(f, "8o7o") == 0.f && dai::moveToUsi(f.bestMove) != "8o7o" &&
            f.provenLossesPruned == 1,
        "forced prior: a proven loss must not be chosen");
  float sum = 0.f;
  for (const auto& kv : f.policy) {
    sum += kv.second;
    if (kv.first == az::moveToIndex(move("8o7o"))) check(kv.second == 0.f, "pruned target");
  }
  check(std::abs(sum - 1.f) < 1e-5f, "pruned policy target sums to one");
  for (int i = 0; i < 200; ++i)
    check(dai::moveToUsi(az::selectSelfplayMove(f, -1, 50, rng)) != "8o7o",
          "sampling never plays a proven loss");
  // Gote's side of the same position: the flipped root.
  auto g = az::Mcts(cfg).search(p, true, {}, 0, uniform, rng);
  check(!az::hangsLastRoyal(p, g.bestMove), "flipped root: hanging move chosen");
  // At the ply cap the game ends before the capture: a draw, not a loss.
  auto cap = az::Mcts(cfg).search(p, false, {}, az::kPlyCap - 1, uniform, rng);
  check(edgeQ(cap, "8o7o") == .5f, "ply cap ends the game before the capture");

  // Root: a winning capture is played at once, with a one-hot policy, even
  // when the network gives it no prior.
  const auto winning =
      position("15/6go8/7ki7/15/15/15/15/15/15/15/15/15/7RO7/15/KI14 b - 1");
  ForcedEval away("15o14o");
  auto w = az::Mcts(cfg).search(winning, false, {}, 0, away, rng);
  check(w.immediateWin && dai::moveToUsi(w.bestMove) == "8m8c", "root plays the royal capture");
  check(w.policy.size() == 1 && w.policy[0].second == 1.f &&
            w.policy[0].first == az::moveToIndex(move("8m8c")) && w.rootQ == 1.f,
        "one-hot policy and won value for the royal capture");
  dai::Move found;
  check(az::findRoyalCapture(winning, dai::generateLegal(winning), &found) &&
            dai::moveToUsi(found) == "8m8c",
        "findRoyalCapture");
  // Capturing one of two royals is not a win.
  const auto two = position("ki14/15/15/15/15/15/7li7/7KI7/7+DE7/15/15/15/1GO13/15/15 w - 1");
  dai::Move both;
  check(az::findRoyalCapture(two, dai::generateLegal(two), &both) &&
            dai::moveToUsi(both) == "8g8h8i",
        "the lion's double capture of king and prince wins");
  const auto kingOnly = position("ki14/15/15/15/15/15/7li7/7KI7/15/15/15/15/1GO13/15/+DE14 w - 1");
  for (const auto& m : dai::generateLegal(kingOnly)) {
    dai::Position after = kingOnly;
    after.apply(m);
    check(after.royals(dai::Sente) > 0, "fixture: no move takes both royals");
  }
  check(!az::findRoyalCapture(kingOnly, dai::generateLegal(kingOnly)),
        "taking the king while the prince survives is not a win");
}

// A real self-play position (tests/data/mate_positions.txt) where Sente
// mates in 5 plies: check, an evasion, a check without evasion, any move,
// the royal capture.
const char* kMate5 =
    "la4gosibtfldk1ir1csla/csstcoirabphde1kibt1st2rc/rc1vofl2krgo3abvovmkn/"
    "1fdrosm1fk2co1sm3BI/4ew5paewfd1ro/5papa1paLIgbpa1papa/1papa12/15/2PA4pa2PAPA3/"
    "PAPA3dh1PA1PA1VM2PA/4PAPAPA1PA3PAPA1/4DE2EW1IRBI2RODK/DK5SI1CO1VO1KNCSFD/"
    "3STFLCOBTPHSIGO3SMRC/1DH3BTKRKIGOABFL1ST1LA b - 1021";

bool mates(const dai::Position& p, const dai::Move& m, int plies) {
  dai::Position c = p;
  c.apply(m);
  dai::MateSolver solver(1);
  solver.setMaxPly(plies);
  return solver.solveAndInternal(c, 100000).result == dai::MateResult::kMate;
}

void testMateSolver() {
  const auto p = position(kMate5);
  az::MctsConfig cfg;
  cfg.sims = 64;
  cfg.batchSize = 8;
  cfg.rootNoise = false;
  cfg.collectStats = true;
  az::RandomEval uniform;
  std::mt19937_64 rng(9);

  // Root mate: played at once, one-hot policy (either real side to move).
  cfg.rootMateNodes = 5000;
  dai::Move mateMove;
  for (bool flip : {false, true}) {
    const auto r = az::Mcts(cfg).search(p, flip, {}, 100, uniform, rng);
    check(r.provenMate && r.mateLength == 3 && r.rootVisits == 0 && r.mateNodes > 0,
          "root mate proven");
    check(r.policy.size() == 1 && r.policy[0].first == az::moveToIndex(r.bestMove) &&
              r.rootQ == 1.f && mates(p, r.bestMove, 4),
          "root mate: one-hot policy on a mating move");
    mateMove = r.bestMove;
  }
  // Not when the line would end after the game cap: 5 plies needed.
  check(!az::Mcts(cfg).search(p, false, {}, az::kPlyCap - 4, uniform, rng).provenMate,
        "root mate beyond the ply cap");
  check(az::Mcts(cfg).search(p, false, {}, az::kPlyCap - 5, uniform, rng).provenMate,
        "root mate just before the ply cap");
  // Not when the PV would repeat a position for the fourth time.
  {
    dai::Position after = p;
    after.apply(mateMove);
    const uint64_t k = az::gameKey(after);
    const std::vector<uint64_t> history{k, 1, k, 2, k};
    check(!az::Mcts(cfg).search(p, false, history, 100, uniform, rng).provenMate,
          "root mate through a fourth occurrence");
    const std::vector<uint64_t> twice{k, 1, k};
    check(az::Mcts(cfg).search(p, false, twice, 100, uniform, rng).provenMate,
          "a third occurrence is fine");
  }

  // In-tree probe, from the defender's side after the mating check: every
  // reply is a proven loss (an evasion runs into the depth-3 mate, any other
  // move loses the royal), so every visited root move scores 0. In the
  // canonical frame the defender is Sente.
  dai::Position after = p;
  after.apply(mateMove);
  const auto defender = after.flipped();
  cfg.rootMateNodes = 0;
  cfg.probeDepth = 3;
  cfg.sims = 512;  // ~111 replies: every one gets visited
  for (bool flip : {false, true}) {
    const auto r = az::Mcts(cfg).search(defender, flip, {}, 101, uniform, rng);
    check(r.probes > 0 && r.probeMates > 0, "probes ran and proved mates");
    for (const auto& m : r.rootMoves) check(m.visits == 0 || m.q == 0.f, "probe: proven loss");
    check(r.rootQ == 0.f, "probe: lost root");
  }
  // Probes stop where the mate would end after the game cap.
  {
    const auto r = az::Mcts(cfg).search(defender, false, {}, az::kPlyCap - 3, uniform, rng);
    check(r.probeMates == 0, "no probe mates beyond the ply cap");
  }
  // Without probes the evasions are not seen as lost.
  cfg.probeDepth = 0;
  const auto plain = az::Mcts(cfg).search(defender, false, {}, 101, uniform, rng);
  bool open = false;
  for (const auto& m : plain.rootMoves) open |= m.visits > 0 && m.q > 0.f;
  check(plain.probes == 0 && plain.mateNodes == 0 && open, "solver off");
}

void testRootRoyalFilter() {
  const auto p = position(kHang);
  az::MctsConfig cfg;
  cfg.sims = 16;
  cfg.batchSize = 16;
  cfg.rootNoise = false;
  cfg.collectStats = true;
  ForcedEval prior("8o7o");
  std::mt19937_64 rng(42);
  auto baseline = az::Mcts(cfg).search(p, false, {}, 0, prior, rng);
  // The visited blunder 8o7o is a proven loss and pruned; the choice falls
  // to the first unvisited move, 8o7n, which hangs too but is not proven.
  check(dai::moveToUsi(baseline.bestMove) == "8o7n" && az::hangsLastRoyal(p, baseline.bestMove),
        "fixture must reproduce an unproven hanging choice");
  cfg.rootRoyalFilter = true;
  auto fixed = az::Mcts(cfg).search(p, false, {}, 0, prior, rng);
  check(!az::hangsLastRoyal(p, fixed.bestMove), "root royal filter");
  check(fixed.royalFilterSkipped > 0 && dai::moveToUsi(fixed.mostVisited) == "8o7o",
        "filter reports the skipped most-visited move");
  check(fixed.policy == baseline.policy && fixed.rootVisits == baseline.rootVisits,
        "the filter must not change visits or the policy target");

  // The same position from Gote's side (flipped root).
  const auto g = p.flipped();
  ForcedEval gotePrior("8o7o");  // canonical frame
  auto flipped = az::Mcts(cfg).search(g.flipped(), true, {}, 0, gotePrior, rng);
  check(!az::hangsLastRoyal(g.flipped(), flipped.bestMove), "royal filter, flipped root");

  // Every move hangs: the filter keeps the most-visited move.
  const auto trapped = position("14ki/15/15/15/15/15/15/15/15/15/15/15/15/2ro12/KI6ro7 b - 1");
  int safe = 0;
  for (const auto& m : dai::generateLegal(trapped)) safe += !az::hangsLastRoyal(trapped, m);
  check(safe == 0, "trapped fixture: every move hangs");
  ForcedEval trappedPrior("15o14o");
  auto lost = az::Mcts(cfg).search(trapped, false, {}, 0, trappedPrior, rng);
  check(lost.royalFilterAllHang, "all-hang flag");
  bool legalChoice = false;
  for (const auto& m : dai::generateLegal(trapped)) legalChoice |= m == lost.bestMove;
  check(legalChoice, "all-hang keeps the search's (legal) choice");
}

}  // namespace

int main() {
  try {
    testSampling();
    testHangsLastRoyal();
    testWinInOne();
    testMateSolver();
    testRootRoyalFilter();
  } catch (const std::exception& e) {
    std::cout << "FAIL: " << e.what() << "\n";
    return 1;
  }
  std::cout << "OK: opening/full-game sampling, cutoff, visit probabilities, policy target, "
               "forced move, hanging-royal, win-in-one terminal, root royal capture, root mate, in-tree mate probe and root royal filter checks passed\n";
  return 0;
}
