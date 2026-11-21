// Game record and termination rules.

#pragma once

#include <string>
#include <vector>

#include "movegen.h"

namespace dai {

enum class Outcome { Ongoing, SenteWins, GoteWins, Draw };

enum class EndReason {
  None,
  RoyalsCaptured,   // the loser has neither king nor prince left
  NoLegalMoves,     // the side to move has no legal move and loses
  Repetition,       // fourth occurrence of a position: draw
  PerpetualCheck,   // fourth occurrence; one side checked the other's royal
                    // on every one of its turns since the first occurrence
                    // and loses
};

struct Status {
  Outcome outcome = Outcome::Ongoing;
  EndReason reason = EndReason::None;
};

std::string describe(const Status& s);   // e.g. "sente wins (royals captured)"
std::string statusToken(const Status& s);  // e.g. "win sente royals"

class Game {
 public:
  explicit Game(const Position& start);

  const Position& current() const { return history_.back(); }
  const Position& initial() const { return history_.front(); }
  const std::vector<Move>& moves() const { return moves_; }
  size_t plies() const { return moves_.size(); }

  // Legal moves in the current position; empty once the game is over
  // (including by repetition).
  std::vector<Move> legalMoves() const;
  Status status() const;

  // Plays a move; returns false (and leaves the game unchanged) if it is not
  // legal in the current position.
  bool play(const Move& m);
  // Plays a move known to be legal.
  void playUnchecked(const Move& m);

 private:
  Status positionStatus() const;  // royal capture and repetition only

  std::vector<Position> history_;
  std::vector<uint64_t> keys_;
  std::vector<bool> checked_;  // royal of the side to move attacked
  std::vector<Move> moves_;
};

}  // namespace dai
