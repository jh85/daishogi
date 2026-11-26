// Capture audit: how much material could each side win, and what did it play?
//
// Reads one game per line on stdin:
//   ID \t INITIAL-SFEN \t USI moves (space separated)
// and writes one line per position (before each move):
//   ID ply stm ncap gain threat played pgain [captures]
// where
//   ncap    legal captures for the side to move;
//   gain    the best material it can win now: a capture-only search (every
//           capture, recapture and re-recapture to --depth plies, either
//           side may stop), HaChu values as in az/material.py; 0 = nothing;
//   threat  the same for the opponent if the side to move passed, i.e. how
//           much of the mover's own material is hanging;
//   played  the move played (USI) and pgain its net gain under the same
//           search (captured material minus the opponent's best answer);
//           "-" for a non-capture;
//   captures  each capture with net gain > 0 as USI:gain, space separated.
// The capture of the last royal counts as kWin. The search is
// dai::ExchangeSearch (src/material.h).
//
// --depth D (default 4) limits the capture search; --every K audits only
// plies 0 and 1 of every K (default: all plies).
//
// --random N --seed S instead plays N games of uniformly random legal moves
// from the initial position (to the end of the game or --max-plies) and
// audits them, with IDs rand_0, rand_1, ...
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/game.h"
#include "../src/material.h"
#include "../src/movegen.h"
#include "../src/notation.h"

using namespace dai;

namespace {

void audit(const std::string& id, const Position& start, const std::vector<Move>& moves,
           int depth, int every) {
  Game game(start);
  for (size_t ply = 0; ply <= moves.size(); ++ply) {
    const Position& pos = game.current();
    // Audit plies 0 and 1 of every block of `every`: the second shows what
    // still hangs after the first's move.
    if (ply % every > 1) {
      if (ply == moves.size()) break;
      game.playUnchecked(moves[ply]);
      continue;
    }
    if (game.status().outcome != Outcome::Ongoing) break;
    ExchangeSearch s;
    s.depth = depth;
    int ncap = 0, gain = 0;
    std::string caps;
    for (const Move& m : generateLegal(pos)) {
      const int c = capturedValue(pos, m);
      if (c < 0) continue;
      ++ncap;
      const int g = s.captureGain(pos, m, c);
      if (g > 0) caps += " " + moveToUsi(m) + ":" + std::to_string(g);
      gain = std::max(gain, g);
    }
    Position passed = pos;  // the opponent to move: what hangs?
    passed.sideToMove = ~pos.sideToMove;
    passed.lionCapture = kNoSquare;
    const int threat = s.gain(passed);
    std::string played = "-", pgain = "-";
    if (ply < moves.size()) {
      played = moveToUsi(moves[ply]);
      const int c = capturedValue(pos, moves[ply]);
      if (c >= 0) {
        pgain = std::to_string(s.captureGain(pos, moves[ply], c));
      }
    }
    std::printf("%s %zu %d %d %d %d %s %s%s\n", id.c_str(), ply, int(pos.sideToMove), ncap,
                gain, threat, played.c_str(), pgain.c_str(), caps.c_str());
    if (ply == moves.size()) break;
    game.playUnchecked(moves[ply]);
  }
}

}  // namespace

int main(int argc, char** argv) {
  int depth = 4, randomGames = 0, maxPlies = 1500, every = 1;
  uint64_t seed = 1;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--depth")) depth = std::atoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--random")) randomGames = std::atoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--seed")) seed = std::strtoull(argv[i + 1], nullptr, 10);
    else if (!std::strcmp(argv[i], "--max-plies")) maxPlies = std::atoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--every")) every = std::max(2, std::atoi(argv[i + 1]));
    else {
      std::fprintf(stderr, "unknown argument %s\n", argv[i]);
      return 2;
    }
  }
  if (randomGames > 0) {
    Position start;
    if (!parseSfen(kStartSfen, start)) return 1;
    std::mt19937_64 rng(seed);
    for (int g = 0; g < randomGames; ++g) {
      Game game(start);
      std::vector<Move> moves;
      while (int(moves.size()) < maxPlies && game.status().outcome == Outcome::Ongoing) {
        const auto legal = game.legalMoves();
        if (legal.empty()) break;
        moves.push_back(legal[std::uniform_int_distribution<size_t>(0, legal.size() - 1)(rng)]);
        game.playUnchecked(moves.back());
      }
      audit("rand_" + std::to_string(g), start, moves, depth, every);
    }
    return 0;
  }
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string id, sfen, movesText;
    if (!std::getline(in, id, '\t') || !std::getline(in, sfen, '\t')) continue;
    std::getline(in, movesText);
    Position start;
    if (!parseSfen(sfen, start)) {
      std::fprintf(stderr, "%s: bad SFEN\n", id.c_str());
      return 1;
    }
    std::vector<Move> moves;
    std::istringstream ms(movesText);
    for (std::string t; ms >> t;) {
      Move m;
      if (!parseUsi(t, m)) {
        std::fprintf(stderr, "%s: bad move %s\n", id.c_str(), t.c_str());
        return 1;
      }
      moves.push_back(m);
    }
    audit(id, start, moves, depth, every);
    std::fflush(stdout);
  }
  return 0;
}
