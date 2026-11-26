// Material: piece values and a capture-only exchange search.
//
// Values are HaChu's (see az/material.py, which must agree): a promoted
// piece is worth the piece it moves as; royals keep HaChu's values.
// tests/az/selfplay_test.py checks the self-play material trajectory
// (materialBalance) against az/material.py.
//
// ExchangeSearch::gain() answers "how much material can the side to move win by
// captures, allowing for the replies?": a negamax over captures only, where
// either side may stop instead of capturing, to `depth` plies. Taking a
// defended pawn with a lion scores 40 - 1000 < 0, so the side stops and the
// answer is 0. Capturing the opponent's last royal scores kExchangeWin.
// Quiet threats, promotions and mates are not seen; it is an estimate.
#pragma once

#include <vector>

#include "position.h"

namespace dai {

constexpr int kExchangeWin = 100000;

int materialValue(Kind k);
// Sente's material minus Gote's.
int materialBalance(const Position& pos);
// Material of one side in the initial position.
int armyValue();

// Material a capture takes (both hops of a double move), kExchangeWin if it
// takes the opponent's last royal, or -1 for a non-capture.
int capturedValue(const Position& pos, const Move& m);

struct ExchangeSearch {
  int depth = 4;
  long nodeLimit = 20000;  // per call; beyond it the search stops capturing
  long nodes = 0;

  // Best net gain (>= 0) of the side to move. `legal` may pass the
  // position's legal moves if already generated.
  int gain(const Position& pos, const std::vector<Move>* legal = nullptr);
  // Net gain of capture m (taking c = capturedValue(pos, m)).
  int captureGain(const Position& pos, const Move& m, int c);

 private:
  int qs(const Position& pos, int depth, int alpha, int beta, const std::vector<Move>* legal);
  int net(const Position& pos, const Move& m, int c, int depth, int alpha, int beta);
};

}  // namespace dai
