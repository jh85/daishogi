// daishogi-az: AlphaZero-side line-protocol tool (self-play worker, match
// player and encoding inspection).
//
// Ported from ../chushogi (src/az/main.cpp, commit d578c81). Left out for
// now: the USI engine mode (usi_mode.*) and the root audit commands.
//
// Usage: daishogi-az [--counter-strike all|lishogi|jcsa] [--promotion chu|honshogi]
//                    [--ply-cap N]  (game draw cap, default 1500; see encode.h)
// (rule variants, see docs/RULES.md; self-play records name both), then
// commands on stdin:
//
//   selfplay --games N --sims S --batch B --seed K --out DIR
//            [--temp-plies T] [--move-temp-early E --move-temp-late L
//             --move-temp-halflife H] [--root-policy-temp X] [--eval random|pipe]
//            [--material-blend L [--material-blend-scale S]]  material bonus
//              in the search's leaf values (MctsConfig::materialBlend)
//            [--capture-prior B]  prior boost for winning captures
//              (MctsConfig::capturePrior)
//            [--port P] [--start startpos|position]
//            [--full-prob F --fast-sims N]   playout cap randomization
//            [--mate-nodes N --probe-depth D --probe-nodes M]  mate solver
//                                             (root budget, in-tree probe; 0 = off)
//                                             play self-play games, write JSON;
//                                             `--start position` starts from the
//                                             current `position` (history dropped)
//   position startpos|sfen <4 fields> [moves <usi...>]  set position -> "ok"
//   seed <n>                                   reseed the match RNG -> "ok"
//   eval-server --port N                       connect to az/eval_server.py
//   go az --sims N [--batch B] [--temp-plies T] [--royal-filter 0|1]
//         [--root-policy-temp X] [--repetition early|fourfold] [--eval random]
//         [--mate-nodes N --probe-depth D --probe-nodes M]
//                                              search -> "bestmove <usi>"
//   analyze az ...                             same options -> "analysis {json}"
//   sfen                                       "sfen <current position>"
//   encode                                     the 114 planes as floats
//   legal-index                                legal moves as policy indices
//                                              (canonical frame)
//   move-index <usi>...                        move -> index
//   index-move <idx>...                        index -> move (USI)
//   quit

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "../game.h"
#include "../notation.h"
#include "encode.h"
#include "eval_client.h"
#include "selfplay.h"

namespace {

[[noreturn]] void fatal(const std::string& msg) {
  std::fprintf(stderr, "error: %s\n", msg.c_str());
  std::exit(1);
}

// Analyze-only instrumentation; normal play uses the original evaluator.
struct TimedEval : az::Evaluator {
  az::Evaluator& base;
  double wallSeconds = 0, cpuSeconds = 0;
  uint64_t calls = 0, positions = 0;
  explicit TimedEval(az::Evaluator& evaluator) : base(evaluator) {}
  template <class F>
  void timed(int n, F&& f) {
    const auto start = std::chrono::steady_clock::now();
    const auto cpu = std::clock();
    f();
    wallSeconds +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    cpuSeconds += double(std::clock() - cpu) / CLOCKS_PER_SEC;
    ++calls;
    positions += n;
  }
  void evaluate(int n, const float* x, float* p, float* v) override {
    timed(n, [&] { base.evaluate(n, x, p, v); });
  }
  void evaluateLegal(int n, const float* x, const std::vector<std::vector<int>>& indices,
                     float* p, float* v) override {
    timed(n, [&] { base.evaluateLegal(n, x, indices, p, v); });
  }
};

std::string usi(const dai::Move& m, bool flip) {
  return dai::moveToUsi(flip ? dai::flipMove(m) : m);
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    dai::CounterStrike r;
    dai::PromotionRule pr;
    if (std::string(argv[i]) == "--counter-strike" && i + 1 < argc &&
        dai::parseCounterStrike(argv[i + 1], r)) {
      dai::setCounterStrike(r);
      ++i;
    } else if (std::string(argv[i]) == "--promotion" && i + 1 < argc &&
               dai::parsePromotionRule(argv[i + 1], pr)) {
      dai::setPromotionRule(pr);
      ++i;
    } else if (std::string(argv[i]) == "--ply-cap" && i + 1 < argc &&
               std::atoi(argv[i + 1]) > 0) {
      az::setPlyCap(std::atoi(argv[i + 1]));
      ++i;
    } else {
      fatal(std::string("unknown argument: ") + argv[i] +
            " (only --counter-strike all|lishogi|jcsa, --promotion chu|honshogi,"
            " --ply-cap N)");
    }
  }
  dai::Position start;
  if (!dai::parseSfen(dai::kStartSfen, start)) fatal("bad initial SFEN");
  dai::Game game(start);
  std::vector<uint64_t> hashes;  // gameKey() of the positions before the current one
  std::unique_ptr<az::PipeEval> pipeEval;
  std::mt19937_64 rng(0xC0FFEE);
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream iss(line);
    std::vector<std::string> tok;
    for (std::string t; iss >> t;) tok.push_back(t);
    if (tok.empty()) continue;
    const dai::Position& pos = game.current();
    if (tok[0] == "quit") break;
    if (tok[0] == "selfplay") {
      int games = 1;
      int port = 51589;
      std::string evalKind = "random";
      az::SelfplayConfig cfg;
      std::string out = "az/data";
      std::string startSfen = dai::kStartSfen;
      for (size_t i = 1; i + 1 < tok.size(); i += 2) {
        if (tok[i] == "--games") games = std::stoi(tok[i + 1]);
        else if (tok[i] == "--start") {  // startpos | position (the `position` command's)
          if (tok[i + 1] == "position") startSfen = dai::toSfen(game.current());
          else if (tok[i + 1] != "startpos") fatal("--start must be startpos or position");
        }
        else if (tok[i] == "--sims") cfg.sims = std::stoi(tok[i + 1]);
        else if (tok[i] == "--batch") cfg.batchSize = std::stoi(tok[i + 1]);
        else if (tok[i] == "--temp-plies") cfg.tempPlies = std::stoi(tok[i + 1]);
        else if (tok[i] == "--move-temp-early") cfg.moveTempEarly = std::stof(tok[i + 1]);
        else if (tok[i] == "--move-temp-late") cfg.moveTempLate = std::stof(tok[i + 1]);
        else if (tok[i] == "--move-temp-halflife") cfg.moveTempHalflife = std::stof(tok[i + 1]);
        else if (tok[i] == "--material-blend") cfg.mcts.materialBlend = std::stof(tok[i + 1]);
        else if (tok[i] == "--material-blend-scale") cfg.mcts.materialScale = std::stof(tok[i + 1]);
        else if (tok[i] == "--capture-prior") cfg.mcts.capturePrior = std::stof(tok[i + 1]);
        else if (tok[i] == "--seed") cfg.seed = std::stoull(tok[i + 1]);
        else if (tok[i] == "--out") out = tok[i + 1];
        else if (tok[i] == "--eval") evalKind = tok[i + 1];
        else if (tok[i] == "--port") port = std::stoi(tok[i + 1]);
        else if (tok[i] == "--root-policy-temp") cfg.mcts.rootPolicyTemperature = std::stof(tok[i + 1]);
        else if (tok[i] == "--full-prob") cfg.fullSearchProb = std::stof(tok[i + 1]);
        else if (tok[i] == "--fast-sims") cfg.fastSims = std::stoi(tok[i + 1]);
        else if (tok[i] == "--mate-nodes") cfg.mcts.rootMateNodes = std::stoull(tok[i + 1]);
        else if (tok[i] == "--probe-depth") cfg.mcts.probeDepth = std::stoi(tok[i + 1]);
        else if (tok[i] == "--probe-nodes") cfg.mcts.probeNodes = std::stoull(tok[i + 1]);
        else fatal("unknown selfplay option: " + tok[i]);
      }
      if (cfg.tempPlies < -1) fatal("--temp-plies must be -1 (all moves) or nonnegative");
      if (!(cfg.moveTempEarly > 0.f && cfg.moveTempLate > 0.f && cfg.moveTempHalflife > 0.f) ||
          !std::isfinite(cfg.moveTempEarly) || !std::isfinite(cfg.moveTempLate) ||
          !std::isfinite(cfg.moveTempHalflife))
        fatal("--move-temp-early, --move-temp-late and --move-temp-halflife must be positive");
      if (cfg.sims <= 0 || cfg.batchSize <= 0) fatal("sims and batch must be positive");
      if (!(cfg.fullSearchProb > 0.f && cfg.fullSearchProb <= 1.f) || cfg.fastSims <= 0)
        fatal("--full-prob must be in (0, 1] and --fast-sims positive");
      if (cfg.mcts.probeDepth < 0 || cfg.mcts.probeNodes == 0)
        fatal("--probe-depth must be nonnegative and --probe-nodes positive");
      cfg.mcts.sims = cfg.sims;
      cfg.mcts.batchSize = cfg.batchSize;
      const std::string mkdir = "mkdir -p '" + out + "'";
      if (std::system(mkdir.c_str()) != 0) fatal("mkdir failed");
      az::RandomEval randomEval;
      std::unique_ptr<az::PipeEval> selfplayEval;
      if (evalKind == "pipe")
        selfplayEval = std::make_unique<az::PipeEval>("127.0.0.1", static_cast<uint16_t>(port));
      else if (evalKind != "random")
        fatal("unknown --eval: " + evalKind);
      az::Evaluator* evalp = selfplayEval ? static_cast<az::Evaluator*>(selfplayEval.get())
                                          : static_cast<az::Evaluator*>(&randomEval);
      for (int g = 0; g < games; ++g) {
        cfg.seed += 1;
        const az::GameRecord rec = az::playGame(startSfen, cfg, *evalp);
        char name[512];
        std::snprintf(name, sizeof(name), "%s/game_%ld_%04d.json", out.c_str(),
                      static_cast<long>(::getpid()), g);
        az::writeGameRecord(rec, name);
        std::cout << "game " << g << ": " << rec.moves.size() << " plies, " << rec.reason
                  << ", result " << rec.result << "\n"
                  << std::flush;
      }
      continue;
    }
    if (tok[0] == "position") {
      size_t i = 1;
      std::string sfen;
      if (i < tok.size() && tok[i] == "startpos") {
        sfen = dai::kStartSfen;
        ++i;
      } else if (i < tok.size() && tok[i] == "sfen") {
        for (++i; i < tok.size() && tok[i] != "moves"; ++i) sfen += (sfen.empty() ? "" : " ") + tok[i];
      } else {
        fatal("expected: position startpos|sfen ...");
      }
      dai::Position p;
      std::string err;
      if (!dai::parseSfen(sfen, p, &err)) fatal("bad SFEN: " + err);
      game = dai::Game(p);
      hashes.clear();
      if (i < tok.size()) {
        if (tok[i] != "moves") fatal("expected: moves");
        for (++i; i < tok.size(); ++i) {
          dai::Move m;
          if (!dai::parseUsi(tok[i], m)) fatal("bad move: " + tok[i]);
          hashes.push_back(az::gameKey(game.current()));
          if (!game.play(m)) fatal("illegal move: " + tok[i]);
        }
      }
      std::cout << "ok\n" << std::flush;
    } else if (tok[0] == "seed") {  // seed <n>: reseed the RNG (match play)
      if (tok.size() != 2) fatal("expected: seed <n>");
      rng.seed(std::stoull(tok[1]));
      std::cout << "ok\n" << std::flush;
    } else if (tok[0] == "eval-server") {  // eval-server --port N
      int port = 51589;
      for (size_t i = 1; i + 1 < tok.size(); i += 2)
        if (tok[i] == "--port") port = std::stoi(tok[i + 1]);
      pipeEval = std::make_unique<az::PipeEval>("127.0.0.1", static_cast<uint16_t>(port));
      std::cout << "ok\n" << std::flush;
    } else if (tok[0] == "go" || tok[0] == "analyze") {
      const bool analyze = tok[0] == "analyze";
      if (tok.size() < 2 || tok[1] != "az") fatal("expected: go az --sims N");
      int sims = 200;
      int tempPlies = 0;  // sample from the visit distribution this many plies
      int repetitionOccurrences = 2;
      int batchSize = 16;
      bool royalFilter = false;
      float rootPolicyTemperature = 1.f;
      size_t mateNodes = 0, probeNodes = 2000;
      int probeDepth = 0;
      std::string evalKind = "pipe";
      for (size_t i = 2; i + 1 < tok.size(); i += 2) {
        if (tok[i] == "--sims") sims = std::stoi(tok[i + 1]);
        else if (tok[i] == "--eval") evalKind = tok[i + 1];
        else if (tok[i] == "--temp-plies") tempPlies = std::stoi(tok[i + 1]);
        else if (tok[i] == "--batch") batchSize = std::stoi(tok[i + 1]);
        else if (tok[i] == "--royal-filter") royalFilter = tok[i + 1] != "0";
        else if (tok[i] == "--root-policy-temp") rootPolicyTemperature = std::stof(tok[i + 1]);
        else if (tok[i] == "--mate-nodes") mateNodes = std::stoull(tok[i + 1]);
        else if (tok[i] == "--probe-depth") probeDepth = std::stoi(tok[i + 1]);
        else if (tok[i] == "--probe-nodes") probeNodes = std::stoull(tok[i + 1]);
        else if (tok[i] == "--repetition") {
          if (tok[i + 1] == "early") repetitionOccurrences = 2;
          else if (tok[i + 1] == "fourfold") repetitionOccurrences = 4;
          else fatal("--repetition must be early or fourfold");
        } else {
          fatal("unknown go option: " + tok[i]);
        }
      }
      if (sims <= 0 || batchSize <= 0) fatal("sims and batch must be positive");
      if (probeDepth < 0 || probeNodes == 0)
        fatal("--probe-depth must be nonnegative and --probe-nodes positive");
      if (!std::isfinite(rootPolicyTemperature) || rootPolicyTemperature <= 0.f)
        fatal("root policy temperature must be finite and positive");
      if (evalKind != "random" && evalKind != "pipe") fatal("unknown --eval: " + evalKind);
      if (evalKind == "pipe" && !pipeEval) fatal("go az needs eval-server first");
      az::RandomEval randomEval;
      az::Evaluator* evalp = evalKind == "random" ? static_cast<az::Evaluator*>(&randomEval)
                                                  : static_cast<az::Evaluator*>(pipeEval.get());
      az::MctsConfig mc;
      mc.sims = sims;
      mc.batchSize = batchSize;
      mc.rootPolicyTemperature = rootPolicyTemperature;
      mc.repetitionOccurrences = repetitionOccurrences;
      mc.collectStats = analyze;
      mc.rootNoise = false;
      mc.rootRoyalFilter = royalFilter;
      mc.rootMateNodes = mateNodes;
      mc.probeDepth = probeDepth;
      mc.probeNodes = probeNodes;
      az::Mcts mcts(mc);
      const bool flip = pos.sideToMove == dai::Gote;
      const dai::Position canon = flip ? pos.flipped() : pos;
      const int plies = static_cast<int>(game.plies());
      TimedEval measured(*evalp);
      const auto searchStart = std::chrono::steady_clock::now();
      const auto searchCpu = std::clock();
      const az::SearchResult res =
          mcts.search(canon, flip, hashes, plies,
                      analyze ? static_cast<az::Evaluator&>(measured) : *evalp, rng);
      const double searchWall =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - searchStart).count();
      const double searchCpuSeconds = double(std::clock() - searchCpu) / CLOCKS_PER_SEC;
      if (analyze) {
        const int repetitions = static_cast<int>(
            std::count(hashes.begin(), hashes.end(), az::gameKey(pos)));
        std::cout << "analysis {\"root_score\":" << res.rootQ
                  << ",\"search_wall_seconds\":" << searchWall
                  << ",\"search_cpu_seconds\":" << searchCpuSeconds
                  << ",\"evaluator_wall_seconds\":" << measured.wallSeconds
                  << ",\"evaluator_cpu_seconds\":" << measured.cpuSeconds
                  << ",\"evaluator_calls\":" << measured.calls
                  << ",\"evaluator_positions\":" << measured.positions
                  << ",\"root_policy_temperature\":" << mc.rootPolicyTemperature
                  << ",\"root_value\":" << res.rootValue << ",\"root_wdl\":["
                  << res.rootWdl[0] << "," << res.rootWdl[1] << "," << res.rootWdl[2] << "]"
                  << ",\"best_move\":\""
                  << (res.bestMove.from == dai::kNoSquare ? "none" : usi(res.bestMove, flip))
                  << "\",\"royal_filter_skipped\":" << res.royalFilterSkipped
                  << ",\"immediate_win\":" << (res.immediateWin ? "true" : "false")
                  << ",\"proven_mate\":" << (res.provenMate ? "true" : "false")
                  << ",\"mate_length\":" << res.mateLength << ",\"mate_nodes\":" << res.mateNodes
                  << ",\"probes\":" << res.probes << ",\"probe_mates\":" << res.probeMates
                  << ",\"proven_losses_pruned\":" << res.provenLossesPruned
                  << ",\"visits\":" << res.rootVisits
                  << ",\"root_prior_occurrences\":" << repetitions << ",\"moves\":[";
        for (size_t i = 0; i < res.rootMoves.size(); ++i) {
          const auto& m = res.rootMoves[i];
          std::cout << (i ? "," : "") << "{\"move\":\"" << usi(m.move, flip)
                    << "\",\"visits\":" << m.visits << ",\"prior\":" << m.prior << ",\"q\":";
          if (m.visits) std::cout << m.q;
          else std::cout << "null";
          float targetMass = 0;
          for (const auto& kv : res.policy)
            if (kv.first == az::moveToIndex(m.move)) targetMass = kv.second;
          std::cout << ",\"policy_target\":" << targetMass
                    << ",\"repetition_visits\":" << m.repetitionVisits
                    << ",\"direct_repetition_draw\":"
                    << (m.directRepetitionDraw ? "true" : "false")
                    << ",\"child_prior_occurrences\":" << m.childPriorOccurrences << "}";
        }
        std::cout << "]}\n" << std::flush;
        continue;
      }
      const auto legal = game.legalMoves();
      if (legal.empty()) {
        std::cout << "bestmove resign\n" << std::flush;
        continue;
      }
      dai::Move best = res.bestMove;
      if (best.from == dai::kNoSquare) best = flip ? dai::flipMove(legal[0]) : legal[0];
      if (plies < tempPlies && !res.policy.empty()) {
        // Match-play temperature: sample from the visit distribution (in the
        // canonical frame, like bestMove) so games between nets diversify.
        std::uniform_real_distribution<float> uni(0.f, 1.f);
        const float pick = uni(rng);
        float acc = 0.f;
        for (const auto& kv : res.policy) {
          acc += kv.second;
          if (pick <= acc + 1e-9f) {
            best = az::indexToMove(kv.first);
            break;
          }
        }
      }
      std::cout << "bestmove " << usi(best, flip) << "\n" << std::flush;
    } else if (tok[0] == "sfen") {
      std::cout << "sfen " << dai::toSfen(pos) << "\n" << std::flush;
    } else if (tok[0] == "encode") {
      const int ply = static_cast<int>(game.plies());
      const int reps = static_cast<int>(std::count(hashes.begin(), hashes.end(), az::gameKey(pos)));
      std::vector<float> planes(az::kNumPlanes * az::kBoard);
      az::encodePosition(pos, reps, ply, planes.data());
      std::cout << "planes";
      for (float v : planes) std::cout << ' ' << v;
      std::cout << "\n" << std::flush;
    } else if (tok[0] == "legal-index") {
      const bool flip = pos.sideToMove == dai::Gote;
      const dai::Position canon = flip ? pos.flipped() : pos;
      std::cout << "legal-index";
      for (const dai::Move& m : dai::generateLegal(canon)) std::cout << ' ' << az::moveToIndex(m);
      std::cout << "\n" << std::flush;
    } else if (tok[0] == "move-index") {
      std::cout << "move-index";
      for (size_t i = 1; i < tok.size(); ++i) {
        dai::Move m;
        if (!dai::parseUsi(tok[i], m)) fatal("bad move: " + tok[i]);
        std::cout << ' ' << az::moveToIndex(m);
      }
      std::cout << "\n" << std::flush;
    } else if (tok[0] == "index-move") {
      std::cout << "index-move";
      for (size_t i = 1; i < tok.size(); ++i) {
        const int idx = std::stoi(tok[i]);
        if (idx < 0 || idx >= az::kPolicySize) fatal("bad index");
        std::cout << ' ' << dai::moveToUsi(az::indexToMove(idx));
      }
      std::cout << "\n" << std::flush;
    } else {
      fatal("unknown command: " + tok[0]);
    }
  }
  return 0;
}
