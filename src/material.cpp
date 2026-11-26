#include "material.h"

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

#include "movegen.h"
#include "notation.h"

namespace dai {

int materialValue(Kind k) {
  static const std::array<int, KindCount> table = [] {
    const std::map<std::string, int> v = {
        {"PA", 40}, {"LA", 150}, {"KN", 60}, {"SI", 100}, {"GO", 151}, {"BI", 250},
        {"RO", 300}, {"KI", 280}, {"DH", 350}, {"DK", 400}, {"LI", 1000}, {"DE", 201},
        {"CO", 100}, {"FL", 150}, {"RC", 150}, {"BT", 152}, {"KR", 154}, {"PH", 153},
        {"GB", 50}, {"SM", 200}, {"VM", 200}, {"FK", 600}, {"IR", 80}, {"ST", 50},
        {"EW", 80}, {"AB", 60}, {"CS", 70}, {"VO", 200}, {"FD", 150},
        // promoted kinds: the value of the piece they move as
        {"+PA", 151}, {"+KN", 151}, {"+IR", 151}, {"+ST", 151}, {"+EW", 151},
        {"+AB", 151}, {"+CS", 151}, {"+VO", 151}, {"+FD", 151}, {"+LA", 350},
        {"+SI", 200}, {"+GO", 300}, {"+BI", 350}, {"+RO", 400}, {"+DH", 500},
        {"+DK", 550}, {"+DE", 270}, {"+CO", 200}, {"+FL", 250}, {"+RC", 250},
        {"+BT", 300}, {"+KR", 1000}, {"+PH", 600}, {"+GB", 201}, {"+SM", 400},
        {"+VM", 400}};
    std::array<int, KindCount> t{};
    for (int k = 0; k < KindCount; ++k) {
      const auto it = v.find(info(static_cast<Kind>(k)).code);
      if (it == v.end())
        throw std::logic_error("no material value for " + info(static_cast<Kind>(k)).code);
      t[k] = it->second;
    }
    return t;
  }();
  return table[k];
}

int materialBalance(const Position& pos) {
  int b = 0;
  for (const Piece p : pos.board)
    if (p) b += (colorOf(p) == Sente ? 1 : -1) * materialValue(kindOf(p));
  return b;
}

int armyValue() {
  static const int army = [] {
    Position start;
    if (!parseSfen(kStartSfen, start)) throw std::logic_error("bad initial SFEN");
    int v = 0;
    for (const Piece p : start.board)
      if (p && colorOf(p) == Sente) v += materialValue(kindOf(p));
    return v;
  }();
  return army;
}

int capturedValue(const Position& pos, const Move& m) {
  const Color them = ~pos.sideToMove;
  int v = -1, royalsTaken = 0;
  for (const int sq : {int(m.mid), int(m.to)}) {
    if (sq == kNoSquare || sq == m.from) continue;
    const Piece p = pos.at(sq);
    if (p && colorOf(p) == them) {
      v = std::max(v, 0) + materialValue(kindOf(p));
      royalsTaken += info(kindOf(p)).royal;
    }
  }
  if (royalsTaken && royalsTaken == pos.royals(them)) return kExchangeWin;
  return v;
}

int ExchangeSearch::gain(const Position& pos, const std::vector<Move>* legal) {
  nodes = 0;
  return qs(pos, depth, -2 * kExchangeWin, 2 * kExchangeWin, legal);
}

int ExchangeSearch::captureGain(const Position& pos, const Move& m, int c) {
  nodes = 0;
  return net(pos, m, c, depth, -2 * kExchangeWin, 2 * kExchangeWin);
}

int ExchangeSearch::qs(const Position& pos, int d, int alpha, int beta,
                       const std::vector<Move>* legal) {
  ++nodes;
  if (d == 0 || nodes > nodeLimit) return 0;
  int best = 0;  // the side may stop
  if (best >= beta) return best;
  alpha = std::max(alpha, best);
  std::vector<std::pair<int, Move>> caps;  // most valuable victim first
  const std::vector<Move> generated = legal ? std::vector<Move>{} : generateLegal(pos);
  for (const Move& m : legal ? *legal : generated) {
    const int c = capturedValue(pos, m);
    if (c >= 0) caps.emplace_back(c, m);
  }
  std::sort(caps.begin(), caps.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  for (const auto& [c, m] : caps) {
    if (c <= alpha) break;  // gains are at most c: nothing left can beat alpha
    const int s = net(pos, m, c, d, c - beta, c - alpha);
    if (s > best) {
      best = s;
      if (best >= beta) break;
      alpha = std::max(alpha, best);
    }
  }
  return best;
}

int ExchangeSearch::net(const Position& pos, const Move& m, int c, int d, int alpha, int beta) {
  if (c >= kExchangeWin) return kExchangeWin;
  Position child = pos;
  child.apply(m);
  return c - qs(child, d - 1, alpha, beta, nullptr);
}

}  // namespace dai
