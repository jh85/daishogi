// Ported from ../chushogi (src/az/mcts.cpp, commit d578c81); see mcts.h for
// what was left out.

#include "mcts.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "../movegen.h"

namespace az {

int positionResult(const dai::Position& pos) {
  const dai::Color us = pos.sideToMove;
  if (pos.royals(us) == 0) return 0;
  if (pos.royals(~us) == 0) return 1;
  if (dai::generateLegal(pos).empty()) return 0;  // no legal move loses
  return -1;
}

bool findRoyalCapture(const dai::Position& pos, const std::vector<dai::Move>& legal,
                      dai::Move* out) {
  const dai::Color them = ~pos.sideToMove;
  if (pos.royals(them) == 0) return false;
  auto theirs = [&](int16_t s) {
    if (s == dai::kNoSquare) return false;
    const dai::Piece p = pos.board[s];
    return p && dai::colorOf(p) == them && dai::info(dai::kindOf(p)).royal;
  };
  for (const dai::Move& m : legal) {
    if (!theirs(m.to) && !theirs(m.mid)) continue;  // captures no royal
    dai::Position after = pos;
    after.apply(m);
    if (after.royals(them) == 0) {
      if (out) *out = m;
      return true;
    }
  }
  return false;
}

bool hangsLastRoyal(const dai::Position& pos, const dai::Move& m) {
  const dai::Color us = pos.sideToMove;
  dai::Position child = pos;
  child.apply(m);
  if (child.royals(us) == 0 || child.royals(~us) == 0) return false;
  if (positionResult(child) >= 0) return false;
  auto ours = [&](int16_t s) {
    if (s == dai::kNoSquare) return false;
    const dai::Piece p = child.board[s];
    return p && dai::colorOf(p) == us && dai::info(dai::kindOf(p)).royal;
  };
  for (const dai::Move& r : dai::generateLegal(child)) {
    if (!ours(r.to) && !ours(r.mid)) continue;  // captures no royal of ours
    dai::Position after = child;
    after.apply(r);
    if (after.royals(us) == 0) return true;
  }
  return false;
}

namespace {

int priorOccurrences(uint64_t hash, const std::vector<uint64_t>& gameHashes,
                     const std::vector<uint64_t>& hashPath, uint64_t rootHash,
                     bool includeRoot) {
  return static_cast<int>(std::count(gameHashes.begin(), gameHashes.end(), hash) +
                          std::count(hashPath.begin(), hashPath.end(), hash)) +
         (includeRoot && hash == rootHash ? 1 : 0);
}

// Early mode (2) scores the first recurrence as a draw. Fourfold mode counts
// real game history, the current root, and earlier search-path nodes.
int terminalOf(const dai::Position& pos, uint64_t hash,
               const std::vector<uint64_t>& gameHashes,
               const std::vector<uint64_t>& hashPath, int totalPly, int maxPlyCap,
               int repetitionOccurrences, uint64_t rootHash, bool& repetitionDraw) {
  const dai::Color us = pos.sideToMove;
  if (pos.royals(us) == 0) return 0;
  if (pos.royals(~us) == 0) return 1;
  const auto legal = dai::generateLegal(pos);
  if (legal.empty()) return 0;          // no legal move loses
  if (totalPly >= maxPlyCap) return 2;  // ply cap: the game ends before this move
  const bool repeated = priorOccurrences(hash, gameHashes, hashPath, rootHash,
                                         repetitionOccurrences == 4) >=
                        repetitionOccurrences - 1;
  // An actual fourfold repetition ends the game before the move; the early
  // cutoff is only an approximation, so an exact win in one beats it.
  if (repeated && repetitionOccurrences == 4) {
    repetitionDraw = true;
    return 2;
  }
  if (findRoyalCapture(pos, legal)) return 1;
  if (repeated) {
    repetitionDraw = true;
    return 2;
  }
  return -1;
}

}  // namespace

dai::MateSolver& Mcts::mateSolver() {
  if (!mate_) mate_ = std::make_unique<dai::MateSolver>(1);
  return *mate_;
}

bool Mcts::rootMate(dai::MateAnswer& out) {
  // The whole line, the final capture (or the defender's last move and the
  // capture) included, must end before the game cap.
  int plyBudget = std::min(cfg_.rootMatePly, cfg_.maxPlyCap - gamePly_);
  while (plyBudget > 0 && gamePly_ + dai::mateHorizon(plyBudget) > cfg_.maxPlyCap) --plyBudget;
  if (cfg_.rootMateNodes == 0 || plyBudget < 3) return false;
  dai::MateSolver& solver = mateSolver();
  solver.setMaxPly(plyBudget);
  out = solver.solve(rootPos_, cfg_.rootMateNodes, true);
  mateNodes_ += out.nodes;
  if (out.result != dai::MateResult::kMate || out.pv.empty()) return false;
  // A fourth occurrence would end the game (fourfold repetition, or
  // perpetual check against the checking attacker) before the mate.
  std::vector<uint64_t> seen{rootKey_};
  dai::Position p = rootPos_;
  for (const dai::Move& m : out.pv) {
    p.apply(m);
    const uint64_t k = realKey(p);
    const auto n = std::count(gameHashes_.begin(), gameHashes_.end(), k) +
                   std::count(seen.begin(), seen.end(), k);
    if (n >= 3) return false;
    seen.push_back(k);
  }
  return true;
}

Mcts::Mcts(MctsConfig cfg) : cfg_(cfg) {
  if (!std::isfinite(cfg_.rootPolicyTemperature) || cfg_.rootPolicyTemperature <= 0.f)
    throw std::invalid_argument("root policy temperature must be finite and positive");
}

uint64_t Mcts::realKey(const dai::Position& pos) const {
  return gameKey(rootFlipped_ ? pos.flipped() : pos);
}

int Mcts::newNode() {
  arena_.emplace_back();
  return static_cast<int>(arena_.size()) - 1;
}

void Mcts::fillNode(int nodeIdx, const dai::Position& pos,
                    const std::vector<dai::Move>& legal, const float* policy,
                    const float* wdl) {
  Node& node = arena_[nodeIdx];
  if (node.evaluated) return;  // duplicate leaves can occur within a batch
  {  // wdl arrives as raw logits: softmax -> [win, draw, loss]
    const float mx = std::max(wdl[0], std::max(wdl[1], wdl[2]));
    const float e0 = std::exp(wdl[0] - mx), e1 = std::exp(wdl[1] - mx),
                e2 = std::exp(wdl[2] - mx);
    const float s = e0 + e1 + e2;
    node.wdl = {{e0 / s, e1 / s, e2 / s}};
    node.leafValue = e0 / s + 0.5f * e1 / s;  // win + draw/2, stm perspective
  }
  std::vector<float> logits;
  logits.reserve(legal.size());
  float maxLogit = -1e30f;
  for (const dai::Move& m : legal) {
    float l = policy[moveToIndex(pos.sideToMove == dai::Gote ? dai::flipMove(m) : m)];
    if (nodeIdx == root_) l /= cfg_.rootPolicyTemperature;
    logits.push_back(l);
    maxLogit = std::max(maxLogit, l);
  }
  float sum = 0.f;
  for (float& l : logits) sum += (l = std::exp(l - maxLogit));
  node.edges.reserve(legal.size());
  for (size_t i = 0; i < legal.size(); ++i) {
    Edge e;
    e.move = legal[i];
    e.policyIndex = moveToIndex(legal[i]);
    e.prior = logits[i] / sum;
    node.edges.push_back(e);
  }
  node.evaluated = true;
}

int Mcts::pickChild(int nodeIdx, bool isRoot) const {
  const Node& node = arena_[nodeIdx];
  const int parentN = std::max(1, node.visits);
  const float cNum = isRoot ? cfg_.cInitRoot : cfg_.cInit;
  const float cBase = isRoot ? cfg_.cBaseRoot : cfg_.cBase;
  const float c = cNum + std::log((parentN + cBase + 1) / cBase);
  const float sqrtN = std::sqrt(static_cast<float>(parentN));
  const float fpuRed = isRoot ? cfg_.fpuReductionRoot : cfg_.fpuReduction;

  float visitedMass = 0.f, sumW = 0.f;
  int32_t sumN = 0;
  for (const Edge& e : node.edges) {
    if (e.n > 0) visitedMass += e.prior;
    sumW += e.w;
    sumN += e.n;
  }
  const float parentQ = sumN > 0 ? sumW / sumN : 0.5f;
  const float fpu = parentQ - fpuRed * std::sqrt(visitedMass);

  int best = -1;
  float bestScore = -1e30f;
  for (size_t i = 0; i < node.edges.size(); ++i) {
    const Edge& e = node.edges[i];
    const float q = e.n + e.inflight > 0 ? e.w / (e.n + e.inflight) : fpu;
    const float u = c * e.prior * sqrtN / (1 + e.n + e.inflight);
    const float score = q + u;
    if (score > bestScore) {
      bestScore = score;
      best = static_cast<int>(i);
    }
  }
  return best;
}

// Descends from the root to an unevaluated/terminal leaf, applying virtual
// loss on the taken edges (undone when the batch is backed up).
Mcts::Descent Mcts::descend() {
  Descent d;
  d.pos = rootPos_;
  int nodeIdx = root_;
  while (true) {
    Node& node = arena_[nodeIdx];
    if (node.terminal >= 0) {  // terminal node: back up its value directly
      d.leaf = nodeIdx;
      d.needsEval = false;
      d.value = node.leafValue;
      return d;
    }
    if (!node.evaluated && nodeIdx != root_) {  // queued for eval already
      d.leaf = nodeIdx;
      d.needsEval = true;
      return d;
    }
    const int ei = pickChild(nodeIdx, nodeIdx == root_);
    Edge& e = node.edges[ei];
    e.inflight++;
    e.n++;
    node.visits++;
    d.path.emplace_back(nodeIdx, ei);
    d.pos.apply(e.move);
    const uint64_t h = realKey(d.pos);
    if (e.child < 0) {
      e.child = newNode();
      Node& child = arena_[e.child];
      child.priorOccurrences = priorOccurrences(h, gameHashes_, d.hashPath, rootKey_,
                                                cfg_.repetitionOccurrences == 4);
      // Repetition: against the game history and earlier in-tree positions
      // (hashPath does not yet contain h).
      const int term = terminalOf(d.pos, h, gameHashes_, d.hashPath,
                                  gamePly_ + static_cast<int>(d.hashPath.size()) + 1,
                                  cfg_.maxPlyCap, cfg_.repetitionOccurrences, rootKey_,
                                  child.repetitionDraw);
      d.hashPath.push_back(h);
      if (term >= 0) {
        child.terminal = static_cast<int8_t>(term);
        child.leafValue = term == 2 ? 0.5f : (term == 1 ? 1.0f : 0.0f);
        child.evaluated = true;
        d.leaf = e.child;
        d.needsEval = false;
        d.value = child.leafValue;
        return d;
      }
      // Shallow mate probe: if the side to move at the child can force the
      // royal capture (within the game cap), the child is a proven win for
      // it and the parent edge gets value 0.
      const int childPly = gamePly_ + static_cast<int>(d.hashPath.size());
      if (cfg_.probeDepth > 0 &&
          childPly + dai::mateHorizon(cfg_.probeDepth) <= cfg_.maxPlyCap) {
        dai::MateSolver& solver = mateSolver();
        solver.setMaxPly(cfg_.probeDepth);
        const dai::MateAnswer ma = solver.solve(d.pos, cfg_.probeNodes, false);
        ++probes_;
        mateNodes_ += ma.nodes;
        if (ma.result == dai::MateResult::kMate) {
          ++probeMates_;
          child.terminal = 1;
          child.leafValue = 1.0f;
          child.evaluated = true;
          d.leaf = e.child;
          d.needsEval = false;
          d.value = 1.0f;
          return d;
        }
      }
      d.leaf = e.child;
      d.needsEval = true;
      return d;
    }
    d.hashPath.push_back(h);
    nodeIdx = e.child;
  }
}

void Mcts::backup(const Descent& d) {
  // d.value is the leaf's value from the leaf's side-to-move perspective;
  // the last edge leads into the leaf, so it receives the flipped value.
  float v = 1.f - d.value;
  for (auto it = d.path.rbegin(); it != d.path.rend(); ++it) {
    Edge& e = arena_[it->first].edges[it->second];
    e.inflight--;  // the virtual visit becomes the real one
    e.w += v;
    if (cfg_.collectStats && arena_[d.leaf].repetitionDraw) ++e.repetitionVisits;
    v = 1.f - v;
  }
}

void Mcts::runBatch(Evaluator& eval) {
  std::vector<Descent> batch;
  batch.reserve(cfg_.batchSize);
  for (int i = 0; i < cfg_.batchSize; ++i) batch.push_back(descend());

  // Evaluate all non-terminal leaves in one call.
  int n = 0;
  for (const Descent& d : batch)
    if (d.needsEval) ++n;
  if (n > 0) {
    std::vector<float> planes(size_t(n) * kNumPlanes * kBoard);
    std::vector<float> policy(size_t(n) * kPolicySize);
    std::vector<float> wdl(size_t(n) * 3);
    std::vector<std::vector<int>> indices(n);
    std::vector<std::vector<dai::Move>> legalMoves(n);
    int j = 0;
    for (const Descent& d : batch) {
      if (!d.needsEval) continue;
      const uint64_t key = realKey(d.pos);
      // hashPath includes the current leaf: exclude it when counting prior
      // occurrences. Early mode counts the real game history only.
      const int repCount =
          cfg_.repetitionOccurrences == 4
              ? priorOccurrences(key, gameHashes_, d.hashPath, rootKey_, true) - 1
              : static_cast<int>(std::count(gameHashes_.begin(), gameHashes_.end(), key));
      encodePosition(d.pos, repCount, gamePly_ + static_cast<int>(d.hashPath.size()),
                     planes.data() + size_t(j) * kNumPlanes * kBoard);
      legalMoves[j] = dai::generateLegal(d.pos);
      for (const auto& move : legalMoves[j])
        indices[j].push_back(
            moveToIndex(d.pos.sideToMove == dai::Gote ? dai::flipMove(move) : move));
      ++j;
    }
    eval.evaluateLegal(n, planes.data(), indices, policy.data(), wdl.data());
    j = 0;
    for (const Descent& d : batch) {
      if (!d.needsEval) continue;
      fillNode(d.leaf, d.pos, legalMoves[j], policy.data() + size_t(j) * kPolicySize,
               wdl.data() + size_t(j) * 3);
      ++j;
    }
  }
  for (Descent& d : batch) {
    if (d.needsEval) d.value = arena_[d.leaf].leafValue;
    backup(d);
  }
}

SearchResult Mcts::search(const dai::Position& root, bool rootFlipped,
                          const std::vector<uint64_t>& gameHashes, int gamePly,
                          Evaluator& eval, std::mt19937_64& rng) {
  arena_.clear();
  gameHashes_ = gameHashes;
  gamePly_ = gamePly;
  rootPos_ = root;
  rootFlipped_ = rootFlipped;
  rootKey_ = realKey(rootPos_);
  mateNodes_ = 0;
  probes_ = probeMates_ = 0;

  SearchResult res;
  root_ = newNode();
  Node& rootNode = arena_[root_];
  // Root terminal check: position rules and the ply cap only — repetition
  // at the root is the game loop's call (in-tree early cutoff would
  // otherwise fire on the *second* occurrence, before the real fourfold).
  const int result = positionResult(rootPos_);
  const int term = result >= 0 ? result : gamePly_ >= cfg_.maxPlyCap ? 2 : -1;
  if (term >= 0) {
    rootNode.terminal = static_cast<int8_t>(term);
    rootNode.leafValue = term == 2 ? 0.5f : (term == 1 ? 1.0f : 0.0f);
    return res;
  }
  {
    // Root evaluation (single-position batch).
    std::vector<float> planes(kNumPlanes * kBoard), policy(kPolicySize), wdl(3);
    const int repCount =
        static_cast<int>(std::count(gameHashes_.begin(), gameHashes_.end(), rootKey_));
    encodePlanes(rootPos_, repCount, gamePly_, planes.data());
    std::vector<std::vector<int>> indices(1);
    const auto legalMoves = dai::generateLegal(rootPos_);
    if (legalMoves.empty()) return res;
    dai::Move win;
    if (findRoyalCapture(rootPos_, legalMoves, &win)) {  // play the winning capture
      res.immediateWin = true;
      res.bestMove = res.mostVisited = win;
      res.policy = {{moveToIndex(win), 1.f}};
      res.rootQ = res.a0gb = res.rootValue = 1.f;
      res.rootWdl = {{1.f, 0.f, 0.f}};
      if (cfg_.collectStats) res.rootMoves.push_back({win, 0, 1.f, 1.f, 0, false, 0});
      return res;
    }
    dai::MateAnswer mate;
    if (rootMate(mate)) {  // play the proven mate's first move
      const dai::Move first = mate.pv.front();
      res.provenMate = true;
      res.mateLength = mate.matePly;
      res.mateNodes = mateNodes_;
      res.bestMove = res.mostVisited = first;
      res.policy = {{moveToIndex(first), 1.f}};
      res.rootQ = res.a0gb = res.rootValue = 1.f;
      res.rootWdl = {{1.f, 0.f, 0.f}};
      if (cfg_.collectStats) res.rootMoves.push_back({first, 0, 1.f, 1.f, 0, false, 0});
      return res;
    }
    for (const auto& move : legalMoves) indices[0].push_back(moveToIndex(move));
    eval.evaluateLegal(1, planes.data(), indices, policy.data(), wdl.data());
    fillNode(root_, rootPos_, legalMoves, policy.data(), wdl.data());
  }
  rootNode.evaluated = true;
  if (rootNode.edges.empty()) return res;

  if (cfg_.rootNoise) {
    std::gamma_distribution<float> gamma(cfg_.dirichletAlpha, 1.0f);
    std::vector<float> noise(rootNode.edges.size());
    float sum = 0.f;
    for (float& x : noise) sum += (x = gamma(rng));
    for (size_t i = 0; i < noise.size(); ++i)
      rootNode.edges[i].prior = (1 - cfg_.dirichletEps) * rootNode.edges[i].prior +
                                cfg_.dirichletEps * noise[i] / sum;
  }

  for (int done = 0; done < cfg_.sims; done += cfg_.batchSize) {
    if (done > 0 && cfg_.stop && cfg_.stop->load(std::memory_order_relaxed)) break;
    runBatch(eval);
  }

  float sumW = 0.f;
  int32_t sumN = 0;
  for (const Edge& e : rootNode.edges) {
    sumW += e.w;
    sumN += e.n;
  }
  res.rootVisits = sumN;
  res.mateNodes = mateNodes_;
  res.probes = probes_;
  res.probeMates = probeMates_;
  res.rootValue = rootNode.leafValue;
  res.rootWdl = rootNode.wdl;
  res.rootQ = sumN > 0 ? sumW / sumN : rootNode.leafValue;
  res.policy.reserve(rootNode.edges.size());
  // Proven losses (the opponent can then take our last royal at once) get
  // no policy-target share and are never chosen, unless every move loses.
  // Visits alone would keep them: a strong wrong prior keeps sending visits
  // to a move whose value is already known to be 0.
  auto provenLoss = [&](const Edge& e) { return e.child >= 0 && arena_[e.child].terminal == 1; };
  bool anySafe = false;
  int32_t safeN = 0;
  float safePrior = 0.f;
  for (const Edge& e : rootNode.edges)
    if (!provenLoss(e)) {
      anySafe = true;
      safeN += e.n;
      safePrior += e.prior;
    }
  const Edge* best = nullptr;
  for (const Edge& e : rootNode.edges) {
    if (cfg_.collectStats) {
      const Node* child = e.child >= 0 ? &arena_[e.child] : nullptr;
      res.rootMoves.push_back({e.move, e.n, e.prior, e.n ? e.w / e.n : 0.5f,
                               e.repetitionVisits, child && child->repetitionDraw,
                               child ? child->priorOccurrences : 0});
    }
    const bool pruned = anySafe && provenLoss(e);
    res.provenLossesPruned += pruned && e.n > 0;
    float p;
    if (pruned) p = 0.f;
    else if (!anySafe) p = sumN ? float(e.n) / sumN : 0.f;
    else if (safeN > 0) p = float(e.n) / safeN;
    else p = safePrior > 0.f ? e.prior / safePrior : 1.f;  // no visits off the losses
    res.policy.emplace_back(e.policyIndex, p);
    if (!pruned && (!best || e.n > best->n || (e.n == best->n && e.prior > best->prior)))
      best = &e;
  }
  if (anySafe && safeN == 0 && safePrior <= 0.f) {  // degenerate prior: uniform
    int safe = 0;
    for (const Edge& e : rootNode.edges) safe += !provenLoss(e);
    for (size_t i = 0; i < rootNode.edges.size(); ++i)
      if (!provenLoss(rootNode.edges[i])) res.policy[i].second = 1.f / safe;
  }
  if (best) res.bestMove = best->move;
  if (best && cfg_.rootRoyalFilter) {
    std::vector<const Edge*> order;
    for (const Edge& e : rootNode.edges) order.push_back(&e);
    std::stable_sort(order.begin(), order.end(),
                     [](const Edge* a, const Edge* b) { return a->n > b->n; });
    res.mostVisited = order.front()->move;
    const Edge* selected = nullptr;
    for (const Edge* e : order) {
      const Node* child = e->child >= 0 ? &arena_[e->child] : nullptr;
      if ((!child || (child->terminal != 0 && child->terminal != 2)) &&
          gamePly_ + 1 < cfg_.maxPlyCap && hangsLastRoyal(rootPos_, e->move)) {
        ++res.royalFilterSkipped;
      } else {
        selected = e;
        break;
      }
    }
    if (selected) res.bestMove = selected->move;
    else res.royalFilterAllHang = true;  // keep the unfiltered choice
  }

  // A0GB target: greedy max-visit descent; the target is the terminal value
  // at a terminal node, else the leaf net eval, negated back to the root's
  // perspective per descended ply.
  {
    int nodeIdx = root_;
    int depth = 0;
    float v = rootNode.leafValue;
    while (true) {
      const Node& n = arena_[nodeIdx];
      if (n.terminal >= 0) {
        v = n.leafValue;
        break;
      }
      const Edge* be = nullptr;
      for (const Edge& e : n.edges)
        if (e.child >= 0 && (!be || e.n > be->n)) be = &e;
      if (!be) break;  // this node was never descended into
      nodeIdx = be->child;
      v = arena_[nodeIdx].leafValue;
      ++depth;
    }
    res.a0gb = (depth % 2) ? 1.f - v : v;
  }
  return res;
}

}  // namespace az
