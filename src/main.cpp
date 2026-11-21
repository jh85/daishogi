// daishogi: dai shogi legal move generator, perft and random-vs-random games.
//
//   daishogi moves  [SFEN|startpos] [USI moves...]   list legal moves
//   daishogi perft  DEPTH [SFEN|startpos] [--divide]
//   daishogi show   [SFEN|startpos] [USI moves...]   print a diagram
//   daishogi play   [--seed N] [--games N] [--max-plies N] [--sfen SFEN]
//                   [--quiet] [--show]               random vs random
//   daishogi batch                                   line protocol on stdin
//
// Any command also takes --counter-strike all|lishogi|jcsa: the 先獅子
// variant (default: all, the haitaka-variants rule; see docs/RULES.md).
//
// See README.md and docs/FORMAT.md.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "game.h"
#include "notation.h"

using namespace dai;

namespace {

// splitmix64 with unbiased bounded draws, so games are reproducible across
// compilers and standard libraries (unlike std::uniform_int_distribution).
class Rng {
 public:
  explicit Rng(uint64_t seed) : s_(seed) {}
  uint64_t next() {
    s_ += 0x9e3779b97f4a7c15ULL;
    uint64_t z = s_;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
  // Uniform integer in [0, n).
  uint64_t below(uint64_t n) {
    const uint64_t limit = UINT64_MAX - UINT64_MAX % n;
    uint64_t v;
    do v = next(); while (v >= limit);
    return v % n;
  }

 private:
  uint64_t s_;
};

// A player that picks uniformly at random among the legal moves.
class RandomPlayer {
 public:
  explicit RandomPlayer(uint64_t seed) : rng_(seed) {}
  Move choose(const std::vector<Move>& legal) {
    return legal[rng_.below(legal.size())];
  }

 private:
  Rng rng_;
};

[[noreturn]] void die(const std::string& msg) {
  std::cerr << "error: " << msg << "\n";
  std::exit(1);
}

Position parsePositionArg(const std::string& arg) {
  Position pos;
  std::string err;
  if (!parseSfen(arg == "startpos" ? kStartSfen : arg, pos, &err))
    die("bad SFEN: " + err);
  return pos;
}

// Builds a game from a start position and USI moves, validating each move.
bool buildGame(const Position& start, const std::vector<std::string>& usi,
               std::unique_ptr<Game>& game, std::string& err) {
  auto g = std::make_unique<Game>(start);
  for (const std::string& t : usi) {
    Move m;
    if (!parseUsi(t, m)) {
      err = "unparsable move " + t;
      return false;
    }
    if (!g->play(m)) {
      err = "illegal move " + t + " in " + toSfen(g->current());
      return false;
    }
  }
  game = std::move(g);
  return true;
}

std::string joinMoves(const std::vector<Move>& moves) {
  std::string s;
  for (const Move& m : moves) {
    if (!s.empty()) s += ' ';
    s += moveToUsi(m);
  }
  return s;
}

// Splits "SFEN|startpos [moves...]" style arguments. A SFEN given as one
// shell argument may contain spaces; it may also be split over up to four
// arguments if they look like SFEN fields.
void positionAndMoves(const std::vector<std::string>& args, size_t i,
                      Position& pos, std::vector<std::string>& moves) {
  std::string sfen = "startpos";
  if (i < args.size()) {
    sfen = args[i++];
    if (sfen != "startpos" && sfen.find(' ') == std::string::npos) {
      // Accept the remaining SFEN fields as separate arguments.
      for (int f = 0; f < 3 && i < args.size(); ++f) {
        const std::string& a = args[i];
        Move m;
        if (f == 0 && a != "b" && a != "w") break;
        if (f > 0 && parseUsi(a, m)) break;
        sfen += " " + a;
        ++i;
      }
    }
  }
  pos = parsePositionArg(sfen);
  moves.assign(args.begin() + static_cast<long>(i), args.end());
}

int cmdMoves(const std::vector<std::string>& args) {
  Position pos;
  std::vector<std::string> usi;
  positionAndMoves(args, 1, pos, usi);
  std::unique_ptr<Game> game;
  std::string err;
  if (!buildGame(pos, usi, game, err)) die(err);
  const std::vector<Move> legal = game->legalMoves();
  std::cout << legal.size() << " legal moves\n";
  for (const Move& m : legal) std::cout << moveToUsi(m) << "\n";
  return 0;
}

int cmdShow(const std::vector<std::string>& args) {
  Position pos;
  std::vector<std::string> usi;
  positionAndMoves(args, 1, pos, usi);
  std::unique_ptr<Game> game;
  std::string err;
  if (!buildGame(pos, usi, game, err)) die(err);
  std::cout << diagram(game->current()) << "sfen " << toSfen(game->current())
            << "\nstatus " << describe(game->status()) << "\n";
  return 0;
}

int cmdPerft(const std::vector<std::string>& args) {
  if (args.size() < 2) die("usage: daishogi perft DEPTH [SFEN|startpos] [--divide]");
  const int depth = std::atoi(args[1].c_str());
  bool divide = false;
  std::vector<std::string> rest;
  for (size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--divide") divide = true;
    else rest.push_back(args[i]);
  }
  rest.insert(rest.begin(), "perft");
  Position pos;
  std::vector<std::string> usi;
  positionAndMoves(rest, 1, pos, usi);
  std::unique_ptr<Game> game;
  std::string err;
  if (!buildGame(pos, usi, game, err)) die(err);
  const auto t0 = std::chrono::steady_clock::now();
  uint64_t total = 0;
  if (divide && depth >= 1) {
    for (const Move& m : generateLegal(game->current())) {
      Position next = game->current();
      next.apply(m);
      const uint64_t n = perft(next, depth - 1);
      std::cout << moveToUsi(m) << ": " << n << "\n";
      total += n;
    }
  } else {
    total = perft(game->current(), depth);
  }
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::cout << "perft " << depth << " = " << total << "  (" << sec << " s, "
            << static_cast<uint64_t>(total / (sec > 0 ? sec : 1e-9)) << " nodes/s)\n";
  return 0;
}

int cmdPlay(const std::vector<std::string>& args) {
  uint64_t seed = 1;
  int games = 1;
  long maxPlies = 0;  // 0 = no limit
  bool quiet = false, show = false;
  std::string sfen = kStartSfen;
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= args.size()) die("missing value for " + a);
      return args[++i];
    };
    if (a == "--seed") seed = std::strtoull(value().c_str(), nullptr, 10);
    else if (a == "--games") games = std::atoi(value().c_str());
    else if (a == "--max-plies") maxPlies = std::atol(value().c_str());
    else if (a == "--sfen") sfen = value();
    else if (a == "--quiet") quiet = true;
    else if (a == "--show") show = true;
    else die("unknown option " + a);
  }
  const Position start = parsePositionArg(sfen);
  std::map<std::string, int> tally;
  for (int g = 0; g < games; ++g) {
    const uint64_t gameSeed = seed + static_cast<uint64_t>(g);
    // Each player draws from its own stream derived from the game seed.
    Rng derive(gameSeed);
    RandomPlayer players[2] = {RandomPlayer(derive.next()), RandomPlayer(derive.next())};
    Game game(start);
    Status st;
    bool cutoff = false;
    while (true) {
      st = game.status();
      if (st.outcome != Outcome::Ongoing) break;
      if (maxPlies > 0 && static_cast<long>(game.plies()) >= maxPlies) {
        cutoff = true;
        break;
      }
      const std::vector<Move> legal = game.legalMoves();
      game.playUnchecked(players[game.current().sideToMove].choose(legal));
    }
    const std::string result = cutoff ? "cutoff (move limit reached; test cutoff, not a game result)"
                                      : describe(st);
    ++tally[cutoff ? "cutoff" : statusToken(st)];
    std::cout << "game " << g + 1 << " seed " << gameSeed << ": " << result
              << " after " << game.plies() << " plies\n";
    if (!quiet) {
      std::cout << "sfen " << toSfen(game.initial()) << "\n";
      std::cout << "moves " << joinMoves(game.moves()) << "\n";
      std::cout << "final " << toSfen(game.current()) << "\n";
    }
    if (show) std::cout << diagram(game.current());
  }
  if (games > 1) {
    std::cout << "summary:";
    for (const auto& [k, v] : tally) std::cout << " [" << k << "] " << v;
    std::cout << "\n";
  }
  return 0;
}

// Line protocol for scripted use (tests, tools). One command per line:
//   position startpos [moves m1 m2 ...]
//   position sfen <board> <b|w> <sq|-> <n> [moves m1 m2 ...]
//   legal        -> "legal m1 m2 ..."         (legal moves of the game)
//   transitions  -> one "<usi> <sfen after the move>" line per legal move,
//                   then "end"
//   sfen         -> "sfen <current position>"
//   status       -> "status ongoing|win <sente|gote> <reason>|draw <reason>"
//   move <usi>   -> "ok" (or "error ...")
//   perft <d>    -> "perft <nodes>"
//   seed <n>     -> "ok"; seeds "go random"
//   go random    -> "bestmove <usi>" or "bestmove none"
//   display      -> diagram
//   quit
int cmdBatch() {
  std::unique_ptr<Game> game = std::make_unique<Game>(parsePositionArg("startpos"));
  Rng rng(1);
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::vector<std::string> tok;
    std::string t;
    while (in >> t) tok.push_back(t);
    if (tok.empty()) continue;
    const std::string& cmd = tok[0];
    if (cmd == "quit") break;
    if (cmd == "position") {
      size_t i = 1;
      std::string sfen;
      if (i < tok.size() && tok[i] == "startpos") {
        sfen = kStartSfen;
        ++i;
      } else if (i < tok.size() && tok[i] == "sfen") {
        ++i;
        while (i < tok.size() && tok[i] != "moves") sfen += (sfen.empty() ? "" : " ") + tok[i++];
      } else {
        std::cout << "error expected startpos or sfen" << std::endl;
        continue;
      }
      std::vector<std::string> usi;
      if (i < tok.size() && tok[i] == "moves") usi.assign(tok.begin() + static_cast<long>(i) + 1, tok.end());
      Position pos;
      std::string err;
      if (!parseSfen(sfen, pos, &err)) {
        std::cout << "error bad sfen: " << err << std::endl;
        continue;
      }
      std::unique_ptr<Game> g;
      if (!buildGame(pos, usi, g, err)) {
        std::cout << "error " << err << std::endl;
        continue;
      }
      game = std::move(g);
    } else if (cmd == "legal") {
      std::string s = "legal";
      for (const Move& m : game->legalMoves()) s += " " + moveToUsi(m);
      std::cout << s << std::endl;
    } else if (cmd == "transitions") {
      std::string s;
      for (const Move& m : game->legalMoves()) {
        Position next = game->current();
        next.apply(m);
        s += moveToUsi(m) + " " + toSfen(next) + "\n";
      }
      std::cout << s << "end" << std::endl;
    } else if (cmd == "sfen") {
      std::cout << "sfen " << toSfen(game->current()) << std::endl;
    } else if (cmd == "status") {
      std::cout << "status " << statusToken(game->status()) << std::endl;
    } else if (cmd == "move" && tok.size() == 2) {
      Move m;
      if (!parseUsi(tok[1], m)) std::cout << "error unparsable move" << std::endl;
      else if (!game->play(m)) std::cout << "error illegal move" << std::endl;
      else std::cout << "ok" << std::endl;
    } else if (cmd == "perft" && tok.size() == 2) {
      std::cout << "perft " << perft(game->current(), std::atoi(tok[1].c_str())) << std::endl;
    } else if (cmd == "seed" && tok.size() == 2) {
      rng = Rng(std::strtoull(tok[1].c_str(), nullptr, 10));
      std::cout << "ok" << std::endl;
    } else if (cmd == "go") {
      const std::vector<Move> legal = game->legalMoves();
      if (legal.empty()) std::cout << "bestmove none" << std::endl;
      else std::cout << "bestmove " << moveToUsi(legal[rng.below(legal.size())]) << std::endl;
    } else if (cmd == "display") {
      std::cout << diagram(game->current()) << std::flush;
    } else {
      std::cout << "error unknown command" << std::endl;
    }
  }
  return 0;
}

void usage() {
  std::cerr <<
      "usage:\n"
      "  daishogi moves  [SFEN|startpos] [USI moves...]\n"
      "  daishogi perft  DEPTH [SFEN|startpos] [USI moves...] [--divide]\n"
      "  daishogi show   [SFEN|startpos] [USI moves...]\n"
      "  daishogi play   [--seed N] [--games N] [--max-plies N] [--sfen SFEN] [--quiet] [--show]\n"
      "  daishogi batch  (line protocol on stdin, see README.md)\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--counter-strike") {
      CounterStrike r;
      if (i + 1 >= argc || !parseCounterStrike(argv[i + 1], r)) {
        std::cerr << "--counter-strike must be all, lishogi or jcsa\n";
        return 2;
      }
      setCounterStrike(r);
      ++i;
      continue;
    }
    args.push_back(argv[i]);
  }
  if (args.empty()) {
    usage();
    return 2;
  }
  const std::string& cmd = args[0];
  if (cmd == "moves") return cmdMoves(args);
  if (cmd == "perft") return cmdPerft(args);
  if (cmd == "show") return cmdShow(args);
  if (cmd == "play") return cmdPlay(args);
  if (cmd == "batch") return cmdBatch();
  usage();
  return 2;
}
