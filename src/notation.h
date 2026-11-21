// Dai shogi SFEN-style positions and USI-style moves. See docs/FORMAT.md.

#pragma once

#include <string>

#include "position.h"

namespace dai {

extern const char* const kStartSfen;

// Parses "<board> <b|w> <lion-capture square|-> <move number>". The last two
// fields are optional (defaults "-" and 1). Returns false and sets `error` on
// malformed input.
bool parseSfen(const std::string& sfen, Position& out, std::string* error = nullptr);
std::string toSfen(const Position& pos);

std::string squareName(int sq);                  // e.g. "8o"
bool parseSquare(const std::string& s, int& out);

std::string moveToUsi(const Move& m);            // "8m8l", "8m8l8k", "3e3d+"
bool parseUsi(const std::string& token, Move& out);

// Piece code with color applied: uppercase for Sente, lowercase for Gote,
// e.g. "+KN" / "+kn".
std::string pieceCode(Piece p);

// Human-readable diagram (ASCII), used by the CLI.
std::string diagram(const Position& pos);

}  // namespace dai
