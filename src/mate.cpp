// Ported from ../chushogi (src/mate.cpp, commit d578c81); see mate.h for
// the model and the changes.

#include "mate.h"

#include <algorithm>
#include <cstdio>

#include "movegen.h"
#include "notation.h"

namespace dai {

// =====================================================================
// Pure branch-number arithmetic (JHBR3/mate/bns.h)
// =====================================================================

namespace bns {

constexpr uint32_t kInf = 1u << 30;

inline uint32_t SatAdd(uint32_t a, uint32_t b) {
  uint64_t s = static_cast<uint64_t>(a) + b;
  return s >= kInf ? kInf : static_cast<uint32_t>(s);
}

struct ChildView {
  uint32_t abn = 1, obn = 1;
};

struct Summary {
  uint32_t abn = 1, obn = 1;
  int best = -1;
  uint32_t second = kInf;
  uint32_t k = 0;
  bool proved = false, disproved = false;
  bool terminal() const { return proved || disproved; }
};

template <bool kOrNode, bool kPnDn>
inline Summary Summarize(const ChildView* c, int n) {
  Summary s;
  uint32_t best_rel = kInf, best_opp = kInf, second = kInf, k = 0;
  uint64_t opp_sum = 0;
  int best = -1;

  for (int i = 0; i < n; i++) {
    const uint32_t rel = kOrNode ? c[i].abn : c[i].obn;
    const uint32_t opp = kOrNode ? c[i].obn : c[i].abn;
    if (rel == 0) {
      s.best = i;
      s.proved = kOrNode;
      s.disproved = !kOrNode;
      if (kOrNode) {
        s.abn = 0;
        s.obn = kInf;
      } else {
        s.abn = kInf;
        s.obn = 0;
      }
      return s;
    }
    if (kPnDn) opp_sum += opp;
    if (rel >= kInf) continue;
    k++;
    if (rel < best_rel || (kPnDn && rel == best_rel && opp < best_opp)) {
      second = best_rel;
      best_rel = rel;
      best_opp = opp;
      best = i;
    } else if (rel < second) {
      second = rel;
    }
  }

  const bool exhausted = kPnDn ? opp_sum == 0 : k == 0;
  if (exhausted) {
    s.proved = !kOrNode;
    s.disproved = kOrNode;
    if (kOrNode) {
      s.abn = kInf;
      s.obn = 0;
    } else {
      s.abn = 0;
      s.obn = kInf;
    }
    return s;
  }

  const uint32_t node_rel = best_rel;
  const uint32_t node_opp =
      kPnDn ? (opp_sum >= kInf ? kInf : static_cast<uint32_t>(opp_sum))
            : SatAdd(best_opp, k - 1);
  s.abn = kOrNode ? node_rel : node_opp;
  s.obn = kOrNode ? node_opp : node_rel;
  s.best = best;
  s.second = second;
  s.k = k;
  return s;
}

template <bool kOrNode>
inline void ChildThresholds(const Summary& s, const ChildView& best, uint32_t abn_th,
                            uint32_t obn_th, uint32_t* child_abn_th,
                            uint32_t* child_obn_th) {
  const uint32_t rel_th = kOrNode ? abn_th : obn_th;
  const uint32_t opp_th = kOrNode ? obn_th : abn_th;
  const uint32_t node_opp = kOrNode ? s.obn : s.abn;
  const uint32_t best_opp = kOrNode ? best.obn : best.abn;
  const uint32_t rel_out = rel_th < SatAdd(s.second, 1) ? rel_th : SatAdd(s.second, 1);
  const uint32_t opp_out = opp_th >= kInf ? kInf : opp_th - (node_opp - best_opp);
  *child_abn_th = kOrNode ? rel_out : opp_out;
  *child_obn_th = kOrNode ? opp_out : rel_out;
}

constexpr ChildView NoMateView() { return ChildView{kInf, 0}; }

}  // namespace bns

// =====================================================================
// Check detection
// =====================================================================

namespace {

// How the piece on `from` reaches `to` on an empty board: 0 = not at all,
// 1 = by a step or jump (never blocked), 2 = along a ray, with the
// direction and distance returned.
int reachOnEmptyBoard(const Position& pos, int from, int to, Dir* dir = nullptr,
                      int* dist = nullptr) {
  const Piece p = pos.board[from];
  const Color c = colorOf(p);
  const KindInfo& ki = info(kindOf(p));
  const int dx = sqX(to) - sqX(from), dy = sqY(to) - sqY(from);
  if (dx == 0 && dy == 0) return 0;
  for (Dir d : ki.steps) {
    const Dir u = physical(d, c);
    if (u.dx == dx && u.dy == dy) return 1;
  }
  for (const Ray& r : ki.rays) {
    const Dir u = physical(Dir{r.dx, r.dy}, c);
    int n;
    if (u.dx != 0) {
      if (dx % u.dx != 0) continue;
      n = dx / u.dx;
    } else {
      if (dx != 0 || dy % u.dy != 0) continue;
      n = dy / u.dy;
    }
    if (n <= 0 || dx != u.dx * n || dy != u.dy * n) continue;
    if (r.limit && n > r.limit) continue;
    if (dir) *dir = u;
    if (dist) *dist = n;
    return 2;
  }
  return 0;
}

std::vector<int16_t> royalSquares(const Position& pos, Color c) {
  std::vector<int16_t> out;
  for (int s = 0; s < kSquares; ++s) {
    const Piece p = pos.board[s];
    if (p && colorOf(p) == c && info(kindOf(p)).royal) out.push_back(int16_t(s));
  }
  return out;
}

}  // namespace

CheckFilter::CheckFilter(const Position& pos) {
  const Color us = pos.sideToMove;
  royals_ = royalSquares(pos, ~us);
  for (int s = 0; s < kSquares; ++s) {
    const Piece p = pos.board[s];
    if (!p || colorOf(p) != us) continue;
    for (int16_t r : royals_) {
      Dir u{};
      int n = 0;
      const int how = reachOnEmptyBoard(pos, s, r, &u, &n);
      if (how == 0) continue;
      Line l{int16_t(s), r, {kNoSquare, kNoSquare}};
      int blockers = 0;  // a step or jump (how == 1) is never blocked
      for (int i = 1; how == 2 && i < n && blockers <= 2; ++i) {
        const int b = makeSq(sqX(s) + u.dx * i, sqY(s) + u.dy * i);
        if (pos.board[b] && blockers++ < 2) l.blocker[blockers - 1] = int16_t(b);
      }
      // A move vacates at most two squares (m.from and a lion's m.mid).
      if (blockers <= 2) lines_.push_back(l);
    }
  }
}

// An attacker after the move is the moved piece (on m.to) or a piece that
// did not move. The latter reach the royal on an empty board, and a move
// vacates at most m.from and m.mid, so a line opens only if each of its
// blockers is one of those. Enemy royals do not move on our move.
bool CheckFilter::givesCheck(const Move& m, const Position& after) const {
  const Color us = ~after.sideToMove;
  for (int16_t r : royals_) {
    const Piece rp = after.board[r];
    if (!rp || colorOf(rp) == us) continue;  // captured
    if (pieceAttacks(after, m.to, r, kNoSquare)) return true;
  }
  for (const Line& l : lines_) {
    if (l.piece == m.to || l.piece == m.from) continue;  // the moved piece: above
    auto vacated = [&](int16_t b) { return b == kNoSquare || b == m.from || b == m.mid; };
    if (!vacated(l.blocker[0]) || !vacated(l.blocker[1])) continue;
    const Piece rp = after.board[l.royal];
    if (!rp || colorOf(rp) == us) continue;
    if (pieceAttacks(after, l.piece, l.royal, kNoSquare)) return true;
  }
  return false;
}

EvasionFilter::EvasionFilter(const Position& pos) {
  const Color us = pos.sideToMove;
  royals_ = royalSquares(pos, us);
  attackers_.resize(royals_.size());
  reach_.resize(royals_.size());
  for (size_t i = 0; i < royals_.size(); ++i) {
    const int r = royals_[i];
    for (int s = 0; s < kSquares; ++s) {
      const Piece p = pos.board[s];
      if (!p || colorOf(p) == us) continue;
      Dir u{};
      int n = 0;
      const int how = reachOnEmptyBoard(pos, s, r, &u, &n);
      if (how == 0) continue;
      reach_[i].push_back(int16_t(s));
      if (!pieceAttacks(pos, s, r, kNoSquare)) continue;
      Attacker a{int16_t(s), {}};
      if (how == 2)
        for (int k = 1; k < n; ++k) a.between.push_back(int16_t(makeSq(sqX(s) + u.dx * k, sqY(s) + u.dy * k)));
      attackers_[i].push_back(std::move(a));
    }
  }
}

bool EvasionFilter::evades(const Move& m, const Position& after) const {
  const Color us = ~after.sideToMove;
  for (size_t i = 0; i < royals_.size(); ++i) {
    const int r = royals_[i];
    if (m.from == r) {  // the royal moved: test its new square in full
      const Piece p = after.board[m.to];
      if (p && colorOf(p) == us && info(kindOf(p)).royal &&
          attackedBy(after, ~us, m.to, kNoSquare))
        return false;
      continue;
    }
    for (const Attacker& a : attackers_[i]) {
      if (a.square == m.to || a.square == m.mid) continue;  // captured
      if (std::find(a.between.begin(), a.between.end(), m.to) == a.between.end())
        return false;  // still attacks
    }
    for (int16_t s : reach_[i]) {
      const Piece p = after.board[s];
      if (p && colorOf(p) != us && pieceAttacks(after, s, r, kNoSquare)) return false;
    }
  }
  return true;
}

// =====================================================================
// MateSolver
// =====================================================================

namespace {

// One child in the search: a move plus its cached state.
struct Child {
  Move move;
  uint64_t hash = 0;
  bns::ChildView view;
  bool pinned = false;  // view fixed locally; never re-probe the TT
  bool rTaint = false;  // resource-tainted no-mate (depth cap, multi-royal)
};

constexpr uint64_t kGolden = 0x9E3779B97F4A7C15ull;

bool capturesOnto(const Position& pos, const Move& m, Color owner) {
  auto royalOf = [&](int16_t s) {
    if (s == kNoSquare) return false;
    const Piece p = pos.board[s];
    return p && colorOf(p) == owner && info(kindOf(p)).royal;
  };
  return royalOf(m.to) || royalOf(m.mid);
}

}  // namespace

MateSolver::MateSolver(size_t ttMb) {
  const size_t want = (ttMb << 20) / sizeof(TTEntry);
  size_t n = size_t{1} << 16;
  while (n * 2 <= want) n *= 2;
  ttMask_ = n - 1;
  tt_.resize(n);
}

MateSolver::TTEntry* MateSolver::probe(uint64_t hash, int ply) {
  const uint64_t mixed = hash + static_cast<uint64_t>(ply) * kGolden;
  TTEntry& e = tt_[mixed & ttMask_];
  return e.tag == mixed && e.gen == gen_ ? &e : nullptr;
}

MateSolver::TTEntry* MateSolver::store(uint64_t hash, int ply) {
  const uint64_t mixed = hash + static_cast<uint64_t>(ply) * kGolden;
  TTEntry& e = tt_[mixed & ttMask_];
  e.tag = mixed;
  e.gen = gen_;
  return &e;
}

template <bool kOrNode, bool kPnDn>
void MateSolver::searchImpl(const Position& pos, uint64_t hash, int ply, uint32_t abnTh,
                            uint32_t obnTh, uint32_t& outAbn, uint32_t& outObn,
                            bool& outTaint) {
  ++nodes_;
  if (ply > maxPlySeen_) maxPlySeen_ = ply;
  const Color us = pos.sideToMove;
  const Color them = ~us;

  if (TTEntry* e = probe(hash, ply)) {
    if (e->abn == 0 || e->obn == 0) {
      outAbn = e->abn;
      outObn = e->obn;
      outTaint = false;
      return;
    }
  }
  auto finish = [&](uint32_t a, uint32_t o, bool taint) {
    if (!taint) {
      TTEntry* e = store(hash, ply);
      e->abn = a;
      e->obn = o;
    }
    outAbn = a;
    outObn = o;
    outTaint = taint;
  };

  // ---- Move filtering / node terminals (see mate.h for the model) ----
  std::vector<Child> children;
  if constexpr (kOrNode) {
    const std::vector<Move> legal = generateLegal(pos);
    // Attacker: checking moves only; capturing the last enemy royal wins.
    for (const Move& m : legal) {
      if (!capturesOnto(pos, m, them)) continue;
      Position c = pos;
      c.apply(m);
      if (c.royals(them) == 0) return finish(0, bns::kInf, false);
    }
    const CheckFilter checks(pos);
    for (const Move& m : legal) {
      Position c = pos;
      c.apply(m);
      if (checks.givesCheck(m, c)) children.push_back(Child{m, c.key(), {}, false, false});
    }
    if (children.empty()) return finish(bns::kInf, 0, false);  // no checks: no mate
  } else {
    // A defender with two royals can sacrifice one and play on: out of
    // scope (unproved, tainted so it never enters the TT).
    if (pos.royals(us) != 1) return finish(bns::kInf, 0, true);
    const std::vector<Move> legal = generateLegal(pos);
    if (legal.empty()) return finish(0, bns::kInf, false);  // no legal move loses
    // Defender: evasions (no own royal attacked after the move), plus any
    // move capturing the attacker's last royal (an immediate escape).
    const EvasionFilter evasions(pos);
    for (const Move& m : legal) {
      Position c = pos;
      c.apply(m);
      if (capturesOnto(pos, m, them) && c.royals(them) == 0) {
        children.push_back(Child{m, c.key(), bns::NoMateView(), true, false});
        continue;
      }
      if (evasions.evades(m, c)) children.push_back(Child{m, c.key(), {}, false, false});
    }
    if (children.empty()) return finish(0, bns::kInf, false);  // the royal is lost
  }

  // ---- Threshold iteration (JHBR3 SearchImpl main loop) ----
  std::vector<bns::ChildView> views(children.size());
  const int n = static_cast<int>(children.size());
  int refreshOnly = -1;
  for (;;) {
    const int lo = refreshOnly < 0 ? 0 : refreshOnly;
    const int hi = refreshOnly < 0 ? n : refreshOnly + 1;
    for (int i = lo; i < hi; ++i) {
      if (children[i].pinned) {
        views[i] = children[i].view;
        continue;
      }
      if (TTEntry* e = probe(children[i].hash, ply + 1))
        views[i] = {e->abn, e->obn};
      else
        views[i] = {1, 1};
    }
    const bns::Summary s = bns::Summarize<kOrNode, kPnDn>(views.data(), n);

    if (s.terminal()) {
      bool taint = false;
      if (s.disproved) {
        if (s.best >= 0)
          taint = children[s.best].rTaint;
        else
          for (const Child& ch : children) taint = taint || ch.rTaint;
      }
      return finish(s.abn, s.obn, taint);
    }

    {
      TTEntry* e = store(hash, ply);
      e->abn = s.abn;
      e->obn = s.obn;
    }

    if (abnTh <= s.abn || obnTh <= s.obn || nodes_ >= nodeLimit_) {
      outAbn = s.abn;
      outObn = s.obn;
      outTaint = false;
      return;
    }

    uint32_t cthA, cthB;
    bns::ChildThresholds<kOrNode>(s, views[s.best], abnTh, obnTh, &cthA, &cthB);
    Child& ch = children[s.best];

    // Route-dependent child terminals: depth cap, then path cycle.
    if (ply + 1 >= maxPly_) {
      ch.view = bns::NoMateView();
      ch.pinned = true;
      ch.rTaint = true;
      refreshOnly = s.best;
      continue;
    }
    if (std::find(path_.begin(), path_.end(), ch.hash) != path_.end()) {
      ch.view = bns::NoMateView();
      ch.pinned = true;
      ch.rTaint = false;
      refreshOnly = s.best;
      continue;
    }

    Position cpos = pos;
    cpos.apply(ch.move);
    path_.push_back(ch.hash);
    uint32_t ra, rb;
    bool rt;
    searchImpl<!kOrNode, kPnDn>(cpos, ch.hash, ply + 1, cthA, cthB, ra, rb, rt);
    path_.pop_back();
    ch.view = {ra, rb};
    ch.rTaint = rt;
    ch.pinned = true;  // keep returned values; tainted ones are not in the TT
    refreshOnly = s.best;
  }
}

bool MateSolver::pvValidates(const Position& root, const MateAnswer& out) const {
  const Color attacker = root.sideToMove;
  const Color defender = ~attacker;
  Position verify = root;
  for (const Move& m : out.pv) {
    if (!isLegal(verify, m)) return false;
    verify.apply(m);
  }
  if (verify.royals(defender) == 0) return true;
  // Otherwise the pv must end with the defender to move and no evasion.
  if (verify.sideToMove != defender || verify.royals(defender) != 1) return false;
  for (const Move& m : generateLegal(verify)) {
    Position c = verify;
    c.apply(m);
    if (c.royals(attacker) == 0 || !royalAttacked(c, defender)) return false;  // escapes
  }
  return true;
}

void MateSolver::extractPvByResolve(const Position& root, MateAnswer& out, size_t budget) {
  // Greedy re-solve walk replicating the search's AND/OR alternation. Each
  // child is re-solved with a ply cap equal to the remaining line budget,
  // taking the child whose proof is shallowest; the budget then shrinks to
  // that proof's depth, so the walk cannot circle. A move leading to a
  // third occurrence along the line is skipped.
  const Color attacker = root.sideToMove;
  const Color defender = ~attacker;
  const int savedMaxPly = maxPly_;
  extractPv_ = false;

  Position pos = root;
  std::vector<Move> pv;
  std::vector<uint64_t> seen{pos.key()};
  auto repeats = [&](uint64_t h) { return std::count(seen.begin(), seen.end(), h) >= 2; };
  int lineBudget = 2 * maxPly_;
  for (int ply = 0; ply < 2 * savedMaxPly; ++ply) {
    const bool attackerTurn = pos.sideToMove == attacker;
    maxPly_ = lineBudget - 1 > 0 ? lineBudget - 1 : 1;
    Move best;
    int bestDepth = 0;
    bool found = false, done = false;
    if (!attackerTurn) {
      bool anyEvasion = false;
      for (const Move& m : generateLegal(pos)) {
        Position c = pos;
        c.apply(m);
        if (c.royals(attacker) == 0) continue;  // would refute the proof
        if (royalAttacked(c, defender)) continue;  // not an evasion
        anyEvasion = true;
        if (repeats(c.key())) continue;
        MateAnswer sub = solveInternal(c, budget);
        if (sub.result != MateResult::kMate) continue;
        if (!found || sub.proofDepth < bestDepth) {
          best = m;
          bestDepth = sub.proofDepth;
          found = true;
        }
      }
      if (!anyEvasion) break;  // mate complete: no evasion
    } else {
      for (const Move& m : generateLegal(pos)) {
        Position c = pos;
        c.apply(m);
        if (c.royals(defender) == 0) {  // capture the last royal
          best = m;
          found = done = true;
          break;
        }
        if (!royalAttacked(c, defender)) continue;  // must give check
        if (repeats(c.key())) continue;
        // The child is the defender to move: ask whether the mate holds.
        MateAnswer sub = solveAndInternal(c, budget);
        if (sub.result != MateResult::kMate) continue;
        if (!found || sub.proofDepth < bestDepth) {
          best = m;
          bestDepth = sub.proofDepth;
          found = true;
        }
      }
    }
    if (!found) {
      pv.clear();
      break;
    }
    pv.push_back(best);
    pos.apply(best);
    seen.push_back(pos.key());
    lineBudget = bestDepth + 1 < lineBudget ? bestDepth + 1 : lineBudget - 1;
    if (done) break;
  }
  maxPly_ = savedMaxPly;
  extractPv_ = true;
  out.pv = std::move(pv);
  out.matePly = static_cast<int>(out.pv.size());
}

void MateSolver::extractPv(const Position& root, MateAnswer& out) {
  Position pos = root;
  std::vector<Move> pv;
  for (int ply = 0; ply < 2 * maxPly_; ++ply) {
    const Color them = ~pos.sideToMove;
    if (pos.royals(them) == 0) break;
    Move chosen;
    bool found = false;
    for (const Move& m : generateLegal(pos)) {
      Position c = pos;
      c.apply(m);
      if (c.royals(them) == 0) {  // immediate capture
        chosen = m;
        found = true;
        break;
      }
      if (TTEntry* e = probe(c.key(), ply + 1)) {
        if (e->abn == 0) {  // proved child
          chosen = m;
          found = true;
          break;
        }
      }
    }
    if (!found) break;
    pv.push_back(chosen);
    pos.apply(chosen);
  }
  out.pv = std::move(pv);
  out.matePly = static_cast<int>(out.pv.size());
}

MateAnswer MateSolver::solve(const Position& root, size_t nodeLimit, bool wantPv) {
  if (root.royals(~root.sideToMove) != 1 || root.royals(root.sideToMove) == 0)
    return MateAnswer{};  // single-royal defenders only (see mate.h)
  const bool saved = extractPv_;
  extractPv_ = wantPv;
  MateAnswer out = solveInternal(root, nodeLimit);
  extractPv_ = saved;
  return out;
}

MateAnswer MateSolver::solveInternal(const Position& root, size_t nodeLimit) {
  return solveRooted<true>(root, nodeLimit);
}

MateAnswer MateSolver::solveAndInternal(const Position& root, size_t nodeLimit) {
  return solveRooted<false>(root, nodeLimit);
}

template <bool kOrRoot>
MateAnswer MateSolver::solveRooted(const Position& root, size_t nodeLimit) {
  if (++gen_ == 0) {  // generation wrapped: really clear once
    std::fill(tt_.begin(), tt_.end(), TTEntry{});
    gen_ = 1;
  }
  path_.clear();
  nodes_ = 0;
  nodeLimit_ = nodeLimit;
  maxPlySeen_ = 0;

  MateAnswer out;
  const uint64_t h = root.key();
  path_.push_back(h);
  uint32_t ra, rb;
  bool rt;
  if (pndn_)
    searchImpl<kOrRoot, true>(root, h, 0, bns::kInf, bns::kInf, ra, rb, rt);
  else
    searchImpl<kOrRoot, false>(root, h, 0, bns::kInf, bns::kInf, ra, rb, rt);

  out.nodes = nodes_;
  out.proofDepth = maxPlySeen_;
  if (ra == 0) {
    out.result = MateResult::kMate;
    if (kOrRoot && extractPv_) {
      extractPv(root, out);
      if (!pvValidates(root, out)) extractPvByResolve(root, out, nodeLimit);
      if (!pvValidates(root, out)) {
        std::fprintf(stderr, "error: mate pv failed self-validation: %s\n",
                     toSfen(root).c_str());
        out.result = MateResult::kUnknown;
        out.pv.clear();
        out.matePly = 0;
      }
    }
  } else if (rb == 0 && !rt) {
    out.result = MateResult::kNoMate;
  }
  return out;
}

}  // namespace dai
