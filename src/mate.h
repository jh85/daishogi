// Dai shogi checkmate solver: BNS (Branch Number Search, after Okabe
// Fumihiro's "route branch number" paper), ported from ../chushogi
// (src/mate.{h,cpp}, commit d578c81), which implements it from the
// reference at JHBR3/mate/bns.{h,cc}. A proof/disproof-number (df-pn)
// arithmetic mode sharing the same engine is included.
//
// What is proved (tsume semantics, sound for the attacker): the side to
// move (attacker) can force the capture of the enemy royal with every
// attacker move giving check (attacking an enemy royal) and every defender
// move saving it. Dai shogi has no check rule, but a single-royal defender
// that does not evade loses on the spot (the royal is captured next move),
// so ignoring non-evasions is sound. A defender move capturing the
// attacker's last royal is an escape, and a defender with no legal move
// loses (dai shogi's rule).
//
// Single-royal defenders only: solve() reports kUnknown for a defender with
// king and prince, and a line on which the defender gains a second royal
// (promoting the drunk elephant) is treated as unproved, since a royal can
// then be sacrificed and the checking/evasion tree cannot model it.
// (Chushogi kept such lines and only gave up when no evasion was left.)
//
// Game-level rules are not modelled: repetition, perpetual check and the
// game's ply cap. Callers bound the line length with setMaxPly: a mate of
// the solver proves a royal capture within mateHorizon(maxPly) plies
// (moves) from the root, the capture included. Defender nodes are searched
// up to ply maxPly - 1, and one without evasion still needs a defender
// move and the capture, so an even maxPly allows one ply more.
//
// Changes from chushogi besides the board: the position key is
// Position::key(); the transposition table is invalidated by a generation
// counter rather than cleared on every solve (cheap in-tree probes); check
// detection at attacker nodes tests only the moved piece and the pieces
// whose lines reach an enemy royal; solve() can skip the PV.

#pragma once

#include <cstdint>
#include <vector>

#include "position.h"

namespace dai {

enum class MateResult { kMate, kNoMate, kUnknown };

inline int mateHorizon(int maxPly) { return maxPly % 2 ? maxPly : maxPly + 1; }

struct MateAnswer {
  MateResult result = MateResult::kUnknown;
  std::vector<Move> pv;  // mating line when kMate (and the PV was requested)
  uint64_t nodes = 0;
  int matePly = 0;       // pv length
  int proofDepth = 0;    // deepest ply the search reached
};

// Which moves of the side to move attack an enemy royal afterwards: the
// moved piece, or a line through the vacated square(s) of the move. Exact
// (it agrees with royalAttacked on the resulting position).
class CheckFilter {
 public:
  explicit CheckFilter(const Position& pos);
  bool givesCheck(const Move& m, const Position& after) const;

 private:
  struct Line {
    int16_t piece, royal;
    int16_t blocker[2];  // the pieces in between (kNoSquare: none)
  };
  std::vector<int16_t> royals_;  // enemy royal squares
  std::vector<Line> lines_;      // own pieces with at most two blockers to a royal
};

// Which moves of the side to move leave none of its royals attacked: a
// non-royal move must capture or block every current attacker, and the
// result is then verified against the pieces that reach a royal on an
// empty board. Exact (agrees with !royalAttacked on the resulting position).
class EvasionFilter {
 public:
  explicit EvasionFilter(const Position& pos);
  bool evades(const Move& m, const Position& after) const;

 private:
  struct Attacker {
    int16_t square;
    std::vector<int16_t> between;  // squares where a piece blocks it
  };
  std::vector<int16_t> royals_;
  std::vector<std::vector<Attacker>> attackers_;  // per royal, current attackers
  std::vector<std::vector<int16_t>> reach_;       // per royal, enemy pieces reaching it
};

class MateSolver {
 public:
  explicit MateSolver(size_t ttMb = 32);

  // Branch-number (BNS) arithmetic when false (default), proof/disproof
  // number (df-pn style) arithmetic when true.
  void setPnDnArithmetic(bool v) { pndn_ = v; }
  void setMaxPly(int p) { maxPly_ = p; }
  int maxPly() const { return maxPly_; }

  MateAnswer solve(const Position& root, size_t nodeLimit, bool wantPv = true);

  // For the PV walk: solve() without the entry guard, and the AND-rooted
  // variant (the root is the defender to move; kMate means the attacker's
  // mate still holds).
  MateAnswer solveInternal(const Position& root, size_t nodeLimit);
  MateAnswer solveAndInternal(const Position& root, size_t nodeLimit);

 private:
  bool pndn_ = false;
  int maxPly_ = 200;
  size_t ttMask_ = 0;
  uint32_t gen_ = 0;

  struct TTEntry {
    uint64_t tag = 0;
    uint32_t abn = 1, obn = 1;
    uint32_t gen = 0;
  };
  std::vector<TTEntry> tt_;

  size_t nodes_ = 0, nodeLimit_ = 0;
  int maxPlySeen_ = 0;
  std::vector<uint64_t> path_;  // keys along the current route
  bool extractPv_ = true;

  template <bool kOrRoot>
  MateAnswer solveRooted(const Position& root, size_t nodeLimit);

  TTEntry* probe(uint64_t hash, int ply);
  TTEntry* store(uint64_t hash, int ply);

  template <bool kOrNode, bool kPnDn>
  void searchImpl(const Position& pos, uint64_t hash, int ply, uint32_t abnTh,
                  uint32_t obnTh, uint32_t& outAbn, uint32_t& outObn, bool& outTaint);

  void extractPv(const Position& root, MateAnswer& out);
  // Fallback when the TT walk cannot reproduce a line: re-solve each child.
  void extractPvByResolve(const Position& root, MateAnswer& out, size_t budget);
  // Legal move by move, and ends in royal capture or a defender without
  // evasion.
  bool pvValidates(const Position& root, const MateAnswer& out) const;
};

}  // namespace dai
