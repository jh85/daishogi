// Mate solver tests (src/mate.h). Chushogi's tests/mate_test.py (commit
// d578c81) checked crafted positions, real game endings and tsume
// problems; with no dai shogi tsume collection, this test instead checks
// the solver against brute force on positions from the last plies of
// self-play games lost by royal capture (tests/data/mate_positions.txt):
//   - CheckFilter agrees with royalAttacked on every legal move;
//   - at depths 3 and 5, the solver's verdict equals an exhaustive search
//     of the same checking/evasion model, for BNS and df-pn arithmetic;
//   - every proved mate's PV replays legally and ends in a royal capture
//     or a defender without evasion, within maxPly plies;
//   - at depth 3, every proved mate is a forced win under the full rules
//     (every defender reply considered, not only evasions);
//   - a reused solver (TT generations) answers like a fresh one;
//   - crafted mates in 1 and 3 for either side, no-mate, and the
//     two-royal defender (unknown).
//
// Usage: tests/mate_test [tests/data/mate_positions.txt]

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/mate.h"
#include "../src/movegen.h"
#include "../src/notation.h"

using namespace dai;

namespace {

void check(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}

Position position(const std::string& s) {
  Position p;
  check(parseSfen(s, p), "SFEN " + s);
  return p;
}

bool terminal(const Position& p) { return p.royals(Sente) == 0 || p.royals(Gote) == 0; }

// ---- Exhaustive search of the solver's model (see mate.h) ----
bool andNode(const Position& pos, int ply, int maxPly, std::vector<uint64_t>& path);

bool orNode(const Position& pos, int ply, int maxPly, std::vector<uint64_t>& path) {
  const Color them = ~pos.sideToMove;
  const auto legal = generateLegal(pos);
  for (const Move& m : legal) {
    Position c = pos;
    c.apply(m);
    if (c.royals(them) == 0) return true;
  }
  if (ply + 1 >= maxPly) return false;
  for (const Move& m : legal) {
    Position c = pos;
    c.apply(m);
    if (!royalAttacked(c, them)) continue;
    if (std::find(path.begin(), path.end(), c.key()) != path.end()) continue;
    path.push_back(c.key());
    const bool win = andNode(c, ply + 1, maxPly, path);
    path.pop_back();
    if (win) return true;
  }
  return false;
}

bool andNode(const Position& pos, int ply, int maxPly, std::vector<uint64_t>& path) {
  const Color us = pos.sideToMove, them = ~us;
  if (pos.royals(us) != 1) return false;
  const auto legal = generateLegal(pos);
  if (legal.empty()) return true;
  std::vector<Position> evasions;
  for (const Move& m : legal) {
    Position c = pos;
    c.apply(m);
    if (c.royals(them) == 0) return false;
    if (!royalAttacked(c, us)) evasions.push_back(c);
  }
  if (evasions.empty()) return true;
  if (ply + 1 >= maxPly) return false;
  for (const Position& c : evasions) {
    if (std::find(path.begin(), path.end(), c.key()) != path.end()) return false;
    path.push_back(c.key());
    const bool win = orNode(c, ply + 1, maxPly, path);
    path.pop_back();
    if (!win) return false;
  }
  return true;
}

bool bruteMate(const Position& pos, int maxPly) {
  std::vector<uint64_t> path{pos.key()};
  return pos.royals(~pos.sideToMove) == 1 && orNode(pos, 0, maxPly, path);
}

// ---- Full rules: the side to move wins within `plies` plies ----
bool fullWin(const Position& pos, int plies);

bool fullLoss(const Position& pos, int plies) {  // side to move loses within plies
  const auto legal = generateLegal(pos);
  if (legal.empty()) return true;
  const Color us = pos.sideToMove;
  for (const Move& m : legal) {
    Position c = pos;
    c.apply(m);
    if (c.royals(~us) == 0) return false;
    if (!fullWin(c, plies)) return false;
  }
  return true;
}

bool fullWin(const Position& pos, int plies) {
  if (plies <= 0) return false;
  const Color them = ~pos.sideToMove;
  const auto legal = generateLegal(pos);
  for (const Move& m : legal) {
    Position c = pos;
    c.apply(m);
    if (c.royals(them) == 0) return true;
  }
  if (plies < 3) return false;
  for (const Move& m : legal) {
    Position c = pos;
    c.apply(m);
    if (fullLoss(c, plies - 2)) return true;
  }
  return false;
}

// The PV's first move wins under the full rules within `plies`.
bool fullWinWith(const Position& pos, const Move& first, int plies) {
  Position c = pos;
  c.apply(first);
  return c.royals(~pos.sideToMove) == 0 || (plies >= 3 && fullLoss(c, plies - 2));
}

bool pvOk(const Position& root, const MateAnswer& a, int maxPly) {
  if (a.pv.empty() || static_cast<int>(a.pv.size()) > maxPly) return false;
  Position p = root;
  for (const Move& m : a.pv) {
    if (!isLegal(p, m)) return false;
    p.apply(m);
  }
  const Color defender = ~root.sideToMove;
  if (p.royals(defender) == 0) return true;
  if (p.sideToMove != defender) return false;
  for (const Move& m : generateLegal(p)) {
    Position c = p;
    c.apply(m);
    if (c.royals(root.sideToMove) == 0 || !royalAttacked(c, defender)) return false;
  }
  return true;
}

void testCrafted(MateSolver& solver) {
  solver.setMaxPly(7);
  struct Case {
    const char* name;
    const char* sfen;
    MateResult want;
    int plies;  // pv length when kMate
  };
  const Case cases[] = {
      {"m1-sente", "7ki7/15/15/15/15/15/15/7RO7/15/15/15/15/15/15/KI14 b - 1",
       MateResult::kMate, 1},
      {"m1-gote", "14ki/15/15/15/15/15/15/7ro7/15/15/15/15/15/15/7KI7 w - 1",
       MateResult::kMate, 1},
      // From the fixture: a check, the only evasions, a mating check.
      {"m5-sente-or-gote", "la4gosibtfldk1ir1csla/csstcoirabphde1kibt1st2rc/rc1vofl2krgo3abvovmkn/1fdrosm1fk2co1sm3BI/4ew5paewfd1ro/5papa1paLIgbpa1papa/1papa12/15/2PA4pa2PAPA3/PAPA3dh1PA1PA1VM2PA/4PAPAPA1PA3PAPA1/4DE2EW1IRBI2RODK/DK5SI1CO1VO1KNCSFD/3STFLCOBTPHSIGO3SMRC/1DH3BTKRKIGOABFL1ST1LA b - 1021",
       MateResult::kMate, 3},
      {"nomate-bare-kings", "7ki7/15/15/15/15/15/15/15/15/15/15/15/15/15/7KI7 b - 1",
       MateResult::kNoMate, 0},
      {"unknown-two-royals", "6+de1ki6/15/15/15/15/15/15/7RO7/15/15/15/15/15/15/KI14 b - 1",
       MateResult::kUnknown, 0},
  };
  for (const Case& c0 : cases)
   for (bool flip : {false, true}) {  // either side as the attacker
    const Case& c = c0;
    const Position p = flip ? position(c.sfen).flipped() : position(c.sfen);
    for (bool pndn : {false, true}) {
      solver.setPnDnArithmetic(pndn);
      const MateAnswer a = solver.solve(p, 100000);
      check(a.result == c.want, std::string(c.name) + ": verdict");
      if (c.want == MateResult::kMate) {
        check(a.matePly == c.plies && pvOk(p, a, 7), std::string(c.name) + ": pv");
        // A PV that ends with the defender out of evasions takes two more
        // plies (any defender move, the capture); full-rule brute force only
        // up to 3 plies (~140^2 positions).
        if (c.plies == 1)
          check(fullWinWith(p, a.pv[0], 3), std::string(c.name) + ": full rules");
      }
      if (c.want != MateResult::kUnknown)
        check((c.want == MateResult::kMate) == bruteMate(p, 7), std::string(c.name) + ": brute");
    }
   }
  solver.setPnDnArithmetic(false);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string path = argc > 1 ? argv[1] : "tests/data/mate_positions.txt";
    std::ifstream in(path);
    check(bool(in), "cannot open " + path);
    std::vector<Position> positions;
    for (std::string line; std::getline(in, line);)
      if (!line.empty() && line[0] != '#') positions.push_back(position(line));
    check(positions.size() >= 100, "too few positions");

    MateSolver solver(4), fresh(4);
    testCrafted(solver);

    // CheckFilter and EvasionFilter vs royalAttacked, on the positions and
    // on every position one move later (where checks are common).
    size_t filterMoves = 0, inCheck = 0;
    auto filters = [&](const Position& p) {
      if (terminal(p)) return;
      const CheckFilter f(p);
      const EvasionFilter e(p);
      inCheck += royalAttacked(p, p.sideToMove);
      for (const Move& m : generateLegal(p)) {
        Position c = p;
        c.apply(m);
        check(f.givesCheck(m, c) == royalAttacked(c, ~p.sideToMove),
              "CheckFilter disagrees: " + toSfen(p) + " " + moveToUsi(m));
        check(e.evades(m, c) == !royalAttacked(c, p.sideToMove),
              "EvasionFilter disagrees: " + toSfen(p) + " " + moveToUsi(m));
        ++filterMoves;
      }
    };
    for (size_t i = 0; i < positions.size(); ++i) {
      filters(positions[i]);
      if (i % 4 || terminal(positions[i])) continue;
      for (const Move& m : generateLegal(positions[i])) {
        Position c = positions[i];
        c.apply(m);
        if (royalAttacked(c, c.sideToMove)) filters(c);  // the defender in check
      }
    }

    int mates3 = 0, mates5 = 0, mates9 = 0, deep9 = 0, longest = 0, compared = 0;
    for (size_t i = 0; i < positions.size(); ++i) {
      const Position& p = positions[i];
      if (terminal(p)) continue;
      solver.setPnDnArithmetic(false);
      solver.setMaxPly(9);
      const bool deepMate = solver.solve(p, 200000, false).result == MateResult::kMate;
      for (int depth : {3, 5, 9}) {
        if (depth == 5 && i % 3) continue;            // depth 5 on a third of them
        if (depth == 9 && !deepMate && i % 50) continue;  // depth 9: mates, and a sample
        const bool want = bruteMate(p, depth);
        for (bool pndn : {false, true}) {
          solver.setPnDnArithmetic(pndn);
          solver.setMaxPly(depth);
          const MateAnswer a = solver.solve(p, 1000000);
          if (depth == 9) ++deep9;
          const std::string where = toSfen(p) + " depth " + std::to_string(depth) +
                                    (pndn ? " pndn" : " bns");
          check((a.result == MateResult::kMate) == want, "verdict vs brute force: " + where);
          if (a.result != MateResult::kMate) continue;
          check(pvOk(p, a, depth), "pv: " + where);
          if (depth == 3) check(fullWinWith(p, a.pv[0], mateHorizon(3)), "full rules: " + where);
          longest = std::max(longest, a.matePly);
          // The flipped position (other side attacking) has the same answer.
          const MateAnswer f = solver.solve(p.flipped(), 1000000);
          check(f.result == MateResult::kMate,
                "flip symmetry: " + where + " flipped " + std::to_string(int(f.result)) + " pv " +
                    std::to_string(a.matePly) + "/" + std::to_string(f.matePly));
          if (getenv("MATE_DUMP") && a.matePly >= 3)
            std::cout << "mate " << a.matePly << " " << toSfen(p) << "\n";
          if (!pndn) ++(depth == 3 ? mates3 : depth == 5 ? mates5 : mates9);
        }
        ++compared;
      }
      // Without PV, from a fresh solver: the same verdict.
      if (i < 300) {
        solver.setPnDnArithmetic(false);
        solver.setMaxPly(3);
        fresh.setMaxPly(3);
        const MateResult r = solver.solve(p, 2000, false).result;
        MateSolver once(1);
        once.setMaxPly(3);
        check(r == once.solve(p, 2000, false).result, "reused vs fresh solver: " + toSfen(p));
      }
    }
    check(mates3 >= 20 && mates5 >= 20 && mates9 >= 50 && longest >= 5, "too few mates in the fixture");
    std::cout << "OK: crafted mates, check/evasion filters on " << filterMoves << " moves ("
              << inCheck << " positions in check), solver = brute force on "
              << compared << " searches (" << mates3 << " mates within 3 plies, " << mates5
              << " within 5, " << mates9 << " of " << deep9 / 2 << " within 9, longest pv " << longest
              << "), PVs, full-rule soundness at depth 3, reused TT\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << "\n";
    return 1;
  }
}
