// Short-mate puzzle finder: reads SFEN lines on stdin, writes one line per
// position with a forced mate of at most --max plies (royal capture
// included; tsume "1-te" = 3 plies, "3-te" = 5, "5-te" = 7):
//   SFEN \t length \t nodes \t legal \t solution moves (space separated)
// Solutions are all first moves that keep a mate of the shortest length.
// --contrast instead writes the positions with at least one checking move
// and no such mate, with length 0 and the checking moves in the last field.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "../src/mate.h"
#include "../src/movegen.h"
#include "../src/notation.h"

using namespace dai;

int main(int argc, char** argv) {
  int maxLen = 7;
  bool contrast = false;
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--contrast")) contrast = true;
  size_t nodes = 200000;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--max")) maxLen = std::atoi(argv[i + 1]);
    if (!std::strcmp(argv[i], "--nodes")) nodes = std::strtoull(argv[i + 1], nullptr, 10);
  }
  MateSolver solver(64);
  std::string line;
  long seen = 0, found = 0, unknown = 0;
  while (std::getline(std::cin, line)) {
    if (line.empty() || line[0] == '#') continue;
    Position pos;
    if (!parseSfen(line, pos)) continue;
    ++seen;
    const Color us = pos.sideToMove, them = ~us;
    const auto legal = generateLegal(pos);
    // Length 1: an enemy royal hangs (a lone one, or the last of two).
    std::vector<Move> sol;
    int len = 0;
    uint64_t used = 0;
    bool exhausted = false;
    for (const Move& m : legal) {
      Position c = pos;
      c.apply(m);
      if (c.royals(them) == 0) sol.push_back(m);
    }
    if (!sol.empty()) len = 1;
    for (int L = 3; !len && L <= maxLen; L += 2) {
      solver.setMaxPly(L);
      MateAnswer a = solver.solve(pos, nodes, false);
      used += a.nodes;
      if (a.result == MateResult::kUnknown && a.nodes >= nodes) { ++unknown; exhausted = true; break; }
      if (a.result != MateResult::kMate) continue;
      len = L;
      for (const Move& m : legal) {
        Position c = pos;
        c.apply(m);
        if (!royalAttacked(c, them)) continue;  // attacker moves must check
        solver.setMaxPly(L - 1);
        MateAnswer s = solver.solveAndInternal(c, nodes);
        used += s.nodes;
        if (s.result == MateResult::kMate) sol.push_back(m);
      }
    }
    if (contrast) {
      if (len || exhausted) continue;
      std::vector<Move> checks;
      for (const Move& m : legal) {
        Position c = pos;
        c.apply(m);
        if (royalAttacked(c, them)) checks.push_back(m);
      }
      if (checks.empty()) continue;
      ++found;
      std::cout << line << "\t0\t" << used << '\t' << legal.size() << '\t';
      for (size_t i = 0; i < checks.size(); ++i) std::cout << (i ? " " : "") << moveToUsi(checks[i]);
      std::cout << '\n';
      continue;
    }
    if (!len) continue;
    ++found;
    std::cout << line << '\t' << len << '\t' << used << '\t' << legal.size() << '\t';
    for (size_t i = 0; i < sol.size(); ++i) std::cout << (i ? " " : "") << moveToUsi(sol[i]);
    std::cout << '\n';
  }
  std::fprintf(stderr, "positions %ld, mates %ld, budget exhausted %ld\n", seen, found, unknown);
}
