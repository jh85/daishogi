#include "movegen.h"

#include <algorithm>

namespace dai {

namespace {

bool isLionPiece(Piece p) { return p && info(kindOf(p)).lion; }

// "Insignificant" first-step victims for the tsukegui rule (付け喰い): a lion
// that took a pawn or go-between on its first step gets no exemption.
bool isPawnOrGoBetween(Piece p) {
  return p && (kindOf(p) == Pawn || kindOf(p) == GoBetween);
}

// A lion on `target` "has a foot" (足のある獅子): it is protected by a piece
// of its own side, judged with the capturing piece lifted from `vacant`
// (so x-rays through the capturer count).
bool lionProtected(const Position& pos, int target, int vacant) {
  return attackedBy(pos, colorOf(pos.board[target]), target, vacant);
}

// 先獅子: after a non-lion took a lion (on `captured`), does the ban protect
// the enemy lion on `to` from a non-lion moving from `from`? `pos` has that
// piece still on `from`. All: every lion. Lishogi: not the lion on the
// capture square. JCSA: only a defended lion, judged with the capturer
// lifted (x-rays through it count). Lions are never restricted by it.
bool counterStrikeProtects(const Position& pos, int captured, int from, int to) {
  switch (counterStrike()) {
    case CounterStrike::Lishogi: return to != captured;
    case CounterStrike::Jcsa: return lionProtected(pos, to, from);
    default: return true;
  }
}

// Capture restrictions on the first (or only) hop from -> to.
bool firstHopAllowed(const Position& pos, int from, int to) {
  const Piece victim = pos.board[to];
  if (!isLionPiece(victim)) return true;
  if (!isLionPiece(pos.board[from]))
    // 先獅子: a non-lion may not take a protected lion right after a
    // non-lion took one.
    return pos.lionCapture == kNoSquare ||
           !counterStrikeProtects(pos, pos.lionCapture, from, to);
  // Lion takes lion: always when adjacent, otherwise only an unprotected one.
  return dist(from, to) == 1 || !lionProtected(pos, to, from);
}

// Capture restrictions on the second hop mid -> to of a two-step move that
// started on `from`. `pos` is the position before the move.
bool secondHopAllowed(const Position& pos, int from, int mid, int to) {
  if (to == from) return true;  // returning to the vacated start square
  const Piece victim = pos.board[to];
  if (!isLionPiece(victim)) return true;
  const Piece first = pos.board[mid];
  if (!isLionPiece(pos.board[from])) {
    // Falcon / eagle: the lion-capture state after the first hop decides.
    if (!isLionPiece(first)) return true;
    // The first hop took a lion on `mid`: 先獅子 is on, with the capturer
    // standing there. (The Lishogi exemption can never apply: to != mid.)
    Position after = pos;
    after.board[mid] = pos.board[from];
    after.board[from] = 0;
    return !counterStrikeProtects(after, mid, mid, to);
  }
  // Lion: a lion adjacent to the start square may always be taken, also on
  // the second step (lishogi / JCSA: adjacency is judged from where the
  // move starts). The reference applies this only to single steps; this is
  // a deliberate difference (docs/RULES.md).
  if (dist(from, to) == 1) return true;
  // After taking a substantial piece first (付け喰い) any lion may be taken;
  // otherwise only an unprotected one.
  if (first && !isPawnOrGoBetween(first)) return true;
  if (!first) return !lionProtected(pos, to, from);
  // First step took a pawn or go-between (lishogi / JCSA semantics):
  // (a) it no longer blocks a defender's line: protection is judged with
  //     both the start square and the first-step square emptied. The
  //     haitaka reference keeps the captured piece as a blocker; this is a
  //     deliberate difference (docs/RULES.md).
  const Color defender = colorOf(victim);
  Position after = pos;
  after.board[from] = 0;
  after.board[mid] = 0;
  if (attackedBy(after, defender, to, kNoSquare)) return false;
  // (b) it still counts as a protector itself (JCSA 2004 supplementary
  //     rules, case 4; lishogi). Pawns and go-betweens only step, so the
  //     emptied squares cannot matter for the other ones.
  for (int s = 0; s < kSquares; ++s) {
    const Piece p = pos.board[s];
    if (s != from && isPawnOrGoBetween(p) && colorOf(p) == defender &&
        pieceAttacks(pos, s, to, from))
      return false;
  }
  return true;
}

// No more than `fileLimit` unpromoted pawns / go-betweens of one side per
// file after the move.
bool fileAllowed(const Position& pos, const Move& m, Kind placed) {
  const int limit = info(placed).fileLimit;
  if (!limit) return true;
  const Color us = pos.sideToMove;
  const Piece mine = makePiece(placed, us);
  const int x = sqX(m.to);
  int count = 1;  // the arriving piece
  for (int y = 0; y < kRanks; ++y) {
    const int s = makeSq(x, y);
    if (s != m.from && pos.board[s] == mine) ++count;
  }
  return count <= limit;
}

}  // namespace

std::vector<Move> generateLegal(const Position& pos) {
  std::vector<Move> out;
  if (pos.royals(Sente) == 0 || pos.royals(Gote) == 0) return out;
  const Color us = pos.sideToMove;
  const int theirRoyals = pos.royals(~us);

  auto add = [&](const Move& m, Kind placed) {
    if (fileAllowed(pos, m, placed)) out.push_back(m);
  };

  std::vector<int> targets;
  for (int s = 0; s < kSquares; ++s) {
    const Piece pc = pos.board[s];
    if (!pc || colorOf(pc) != us) continue;
    const Kind kind = kindOf(pc);
    const KindInfo& ki = info(kind);
    const int sx = sqX(s), sy = sqY(s);

    // Destinations reachable in one step, jump or slide (not own pieces).
    targets.clear();
    for (Dir d : ki.steps) {
      Dir u = physical(d, us);
      int x = sx + u.dx, y = sy + u.dy;
      if (!onBoard(x, y)) continue;
      int t = makeSq(x, y);
      if (!pos.board[t] || colorOf(pos.board[t]) != us) targets.push_back(t);
    }
    for (const Ray& r : ki.rays) {
      Dir u = physical(Dir{r.dx, r.dy}, us);
      int x = sx + u.dx, y = sy + u.dy;
      for (int n = 1; onBoard(x, y) && (!r.limit || n <= r.limit); ++n) {
        int t = makeSq(x, y);
        const Piece occ = pos.board[t];
        if (!occ || colorOf(occ) != us) targets.push_back(t);
        if (occ) break;
        x += u.dx;
        y += u.dy;
      }
    }

    for (int t : targets) {
      if (!firstHopAllowed(pos, s, t)) continue;
      // Sente-POV vector of the step, for the continuation lookup.
      Dir delta{static_cast<int8_t>(sqX(t) - sx), static_cast<int8_t>(sqY(t) - sy)};
      if (us == Gote) delta = physical(delta, Gote);
      const std::vector<Dir>* follow = continuation(kind, delta);

      // Promotion: optional when the move starts or ends in the far five
      // ranks; compulsory where the unpromoted piece could not move again.
      const bool can = ki.promotesTo >= 0 &&
                       (relRank(s, us) < kPromotionRanks ||
                        relRank(t, us) < kPromotionRanks);
      const bool must = can && relRank(t, us) < ki.deadRanks;
      const Move single{static_cast<int16_t>(s), kNoSquare, static_cast<int16_t>(t), false};
      if (!must) add(single, kind);
      if (can && !follow) {
        Move pm = single;
        pm.promote = true;
        add(pm, static_cast<Kind>(ki.promotesTo));
      }
      if (!follow) continue;

      // Two-step moves. No continuation once the first step has captured
      // the opponent's last royal piece (the game is already won).
      const Piece victim = pos.board[t];
      if (victim && info(kindOf(victim)).royal && theirRoyals == 1) continue;
      for (Dir d : *follow) {
        Dir u = physical(d, us);
        int x = sqX(t) + u.dx, y = sqY(t) + u.dy;
        if (!onBoard(x, y)) continue;
        int e = makeSq(x, y);
        if (e != s && pos.board[e] && colorOf(pos.board[e]) == us) continue;
        if (!secondHopAllowed(pos, s, t, e)) continue;
        add(Move{static_cast<int16_t>(s), static_cast<int16_t>(t),
                 static_cast<int16_t>(e), false},
            kind);
      }
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool isLegal(const Position& pos, const Move& m) {
  const std::vector<Move> moves = generateLegal(pos);
  return std::binary_search(moves.begin(), moves.end(), m);
}

uint64_t perft(const Position& pos, int depth) {
  if (depth == 0) return 1;
  const std::vector<Move> moves = generateLegal(pos);
  if (depth == 1) return moves.size();
  uint64_t n = 0;
  for (const Move& m : moves) {
    Position next = pos;
    next.apply(m);
    n += perft(next, depth - 1);
  }
  return n;
}

}  // namespace dai
