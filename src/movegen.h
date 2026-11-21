// Legal move generation for dai shogi (Shogitter rule 49 semantics).

#pragma once

#include <vector>

#include "position.h"

namespace dai {

// All legal moves of the side to move, sorted and without duplicates.
// Empty when either side has no royal piece left (the game is over).
// Repetition is a property of the game history, not of the position; see
// game.h for the game-level termination rules.
std::vector<Move> generateLegal(const Position& pos);

bool isLegal(const Position& pos, const Move& m);

// Number of leaf nodes at the given depth (no repetition handling).
uint64_t perft(const Position& pos, int depth);

}  // namespace dai
