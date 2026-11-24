#include "position.h"

namespace dai {

namespace {

struct Zobrist {
  uint64_t piece[kSquares][2 * KindCount + 1];
  uint64_t side;
  uint64_t lionCapture;
  uint64_t lionSquare[kSquares];  // Lishogi variant: the square matters
  Zobrist() {
    uint64_t s = 0x2545f4914f6cdd1dULL;
    auto next = [&] {
      s += 0x9e3779b97f4a7c15ULL;
      uint64_t z = s;
      z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
      z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
      return z ^ (z >> 31);
    };
    for (auto& sq : piece)
      for (auto& v : sq) v = next();
    side = next();
    lionCapture = next();
    for (auto& z : lionSquare) z = next();
  }
};

const Zobrist& zobrist() {
  static const Zobrist z;
  return z;
}

}  // namespace

#ifndef DAI_COUNTER_STRIKE
#define DAI_COUNTER_STRIKE 0
#endif
static_assert(DAI_COUNTER_STRIKE >= 0 && DAI_COUNTER_STRIKE <= 2,
              "DAI_COUNTER_STRIKE must be 0 (all), 1 (lishogi) or 2 (jcsa)");
CounterStrike gCounterStrike = static_cast<CounterStrike>(DAI_COUNTER_STRIKE);

bool parseCounterStrike(const std::string& name, CounterStrike& out) {
  if (name == "all") out = CounterStrike::All;
  else if (name == "lishogi") out = CounterStrike::Lishogi;
  else if (name == "jcsa") out = CounterStrike::Jcsa;
  else return false;
  return true;
}

const char* counterStrikeName(CounterStrike r) {
  switch (r) {
    case CounterStrike::Lishogi: return "lishogi";
    case CounterStrike::Jcsa: return "jcsa";
    default: return "all";
  }
}

#ifndef DAI_PROMOTION
#define DAI_PROMOTION 0
#endif
static_assert(DAI_PROMOTION >= 0 && DAI_PROMOTION <= 1,
              "DAI_PROMOTION must be 0 (chu) or 1 (normal)");
PromotionRule gPromotionRule = static_cast<PromotionRule>(DAI_PROMOTION);

bool parsePromotionRule(const std::string& name, PromotionRule& out) {
  if (name == "chu") out = PromotionRule::Chu;
  else if (name == "normal") out = PromotionRule::Normal;
  else return false;
  return true;
}

const char* promotionRuleName(PromotionRule r) {
  return r == PromotionRule::Normal ? "normal" : "chu";
}

void Position::apply(const Move& m) {
  auto hop = [&](int a, int b) {
    const Piece mover = board[a];
    const Piece victim = board[b];
    lionCapture = (victim && info(kindOf(victim)).lion &&
                   !info(kindOf(mover)).lion)
                      ? static_cast<int16_t>(b)
                      : static_cast<int16_t>(kNoSquare);
    board[a] = 0;
    board[b] = mover;
  };
  if (m.mid != kNoSquare) {
    hop(m.from, m.mid);
    hop(m.mid, m.to);
  } else {
    hop(m.from, m.to);
  }
  if (m.promote) {
    const Piece p = board[m.to];
    board[m.to] = makePiece(static_cast<Kind>(info(kindOf(p)).promotesTo),
                            colorOf(p));
  }
  sideToMove = ~sideToMove;
  ++moveNumber;
}

int Position::royals(Color c) const {
  int n = 0;
  for (Piece p : board)
    if (p && colorOf(p) == c && info(kindOf(p)).royal) ++n;
  return n;
}

Position Position::flipped() const {
  Position out;
  for (int s = 0; s < kSquares; ++s) out.board[flipSquare(s)] = flipPiece(board[s]);
  out.sideToMove = ~sideToMove;
  out.lionCapture = lionCapture == kNoSquare
                        ? static_cast<int16_t>(kNoSquare)
                        : static_cast<int16_t>(flipSquare(lionCapture));
  out.moveNumber = moveNumber;
  return out;
}

uint64_t Position::key() const {
  const Zobrist& z = zobrist();
  uint64_t k = 0;
  for (int s = 0; s < kSquares; ++s)
    if (board[s]) k ^= z.piece[s][board[s]];
  if (sideToMove == Gote) k ^= z.side;
  if (lionCapture != kNoSquare)
    k ^= counterStrike() == CounterStrike::Lishogi ? z.lionSquare[lionCapture] : z.lionCapture;
  return k;
}

bool pieceAttacks(const Position& pos, int from, int to, int vacant) {
  const Piece p = pos.board[from];
  const Color c = colorOf(p);
  const KindInfo& ki = info(kindOf(p));
  const int fx = sqX(from), fy = sqY(from);
  const int dx = sqX(to) - fx, dy = sqY(to) - fy;
  if (dx == 0 && dy == 0) return false;
  for (Dir d : ki.steps) {
    Dir u = physical(d, c);
    if (u.dx == dx && u.dy == dy) return true;
  }
  for (const Ray& r : ki.rays) {
    Dir u = physical(Dir{r.dx, r.dy}, c);
    // `to` must lie on the ray: to = from + n * u with n >= 1.
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
    bool blocked = false;
    for (int i = 1; i < n; ++i) {
      int s = makeSq(fx + u.dx * i, fy + u.dy * i);
      if (s != vacant && pos.board[s]) {
        blocked = true;
        break;
      }
    }
    if (!blocked) return true;
  }
  return false;
}

bool attackedBy(const Position& pos, Color by, int target, int vacant) {
  for (int s = 0; s < kSquares; ++s) {
    if (s == vacant) continue;
    const Piece p = pos.board[s];
    if (p && colorOf(p) == by && pieceAttacks(pos, s, target, vacant))
      return true;
  }
  return false;
}

bool royalAttacked(const Position& pos, Color c) {
  for (int s = 0; s < kSquares; ++s) {
    const Piece p = pos.board[s];
    if (p && colorOf(p) == c && info(kindOf(p)).royal &&
        attackedBy(pos, ~c, s, kNoSquare))
      return true;
  }
  return false;
}

}  // namespace dai
