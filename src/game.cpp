#include "game.h"

namespace dai {

namespace {
const char* sideName(Outcome o) {
  return o == Outcome::SenteWins ? "sente" : "gote";
}
const char* reasonName(EndReason r) {
  switch (r) {
    case EndReason::RoyalsCaptured: return "royals";
    case EndReason::NoLegalMoves: return "nomoves";
    case EndReason::Repetition: return "repetition";
    case EndReason::PerpetualCheck: return "perpetual";
    default: return "none";
  }
}
}  // namespace

std::string describe(const Status& s) {
  switch (s.reason) {
    case EndReason::None: return "ongoing";
    case EndReason::RoyalsCaptured:
      return std::string(sideName(s.outcome)) + " wins (all enemy royals captured)";
    case EndReason::NoLegalMoves:
      return std::string(sideName(s.outcome)) + " wins (opponent has no legal move)";
    case EndReason::Repetition: return "draw (fourfold repetition)";
    case EndReason::PerpetualCheck:
      return std::string(sideName(s.outcome)) + " wins (opponent gave perpetual check)";
  }
  return "?";
}

std::string statusToken(const Status& s) {
  if (s.outcome == Outcome::Ongoing) return "ongoing";
  if (s.outcome == Outcome::Draw) return std::string("draw ") + reasonName(s.reason);
  return std::string("win ") + sideName(s.outcome) + " " + reasonName(s.reason);
}

Game::Game(const Position& start) {
  history_.push_back(start);
  keys_.push_back(start.key());
  checked_.push_back(royalAttacked(start, start.sideToMove));
}

Status Game::positionStatus() const {
  const Position& p = current();
  if (p.royals(Sente) == 0) return {Outcome::GoteWins, EndReason::RoyalsCaptured};
  if (p.royals(Gote) == 0) return {Outcome::SenteWins, EndReason::RoyalsCaptured};

  // Repetition: the current position occurs for the fourth time.
  const uint64_t k = keys_.back();
  const size_t last = history_.size() - 1;
  size_t first = last, count = 0;
  for (size_t i = 0; i <= last; ++i)
    if (keys_[i] == k && history_[i].sameState(p)) {
      if (count++ == 0) first = i;
    }
  if (count < 4) return {};
  // Perpetual check: if, in every position after the first occurrence where
  // side V was to move, V's royal was attacked, the checking side loses.
  for (Color victim : {p.sideToMove, ~p.sideToMove}) {
    bool all = true;
    for (size_t i = first + 1; i <= last && all; ++i)
      if (history_[i].sideToMove == victim && !checked_[i]) all = false;
    if (all)
      return {victim == Sente ? Outcome::SenteWins : Outcome::GoteWins,
              EndReason::PerpetualCheck};
  }
  return {Outcome::Draw, EndReason::Repetition};
}

std::vector<Move> Game::legalMoves() const {
  if (positionStatus().outcome != Outcome::Ongoing) return {};
  return generateLegal(current());
}

Status Game::status() const {
  Status s = positionStatus();
  if (s.outcome != Outcome::Ongoing) return s;
  if (generateLegal(current()).empty())
    return {current().sideToMove == Sente ? Outcome::GoteWins : Outcome::SenteWins,
            EndReason::NoLegalMoves};
  return s;
}

bool Game::play(const Move& m) {
  if (positionStatus().outcome != Outcome::Ongoing) return false;
  if (!isLegal(current(), m)) return false;
  playUnchecked(m);
  return true;
}

void Game::playUnchecked(const Move& m) {
  Position next = current();
  next.apply(m);
  keys_.push_back(next.key());
  checked_.push_back(royalAttacked(next, next.sideToMove));
  history_.push_back(next);
  moves_.push_back(m);
}

}  // namespace dai
