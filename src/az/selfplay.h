// AlphaZero self-play game loop and record writing.
//
// Ported from ../chushogi (src/az/selfplay.h, commit d578c81).
//
// A game is played from an initial SFEN; each ply runs MCTS on the
// canonicalized position, records the per-position training sample (packed
// planes + visit policy + the three value targets), and plays a move
// (temperature-sampled for the first plies, argmax after). The game is
// adjudicated by dai::Game (royal capture, no legal move, fourfold
// repetition, perpetual check) plus the kPlyCap draw.
//
// Game records are JSON (initial SFEN + USI moves + samples), readable by
// the Python replay buffer and replayable with ./daishogi.

#pragma once

#include <string>
#include <vector>

#include "../position.h"
#include "mcts.h"

namespace az {

struct Sample {
  int ply = 0;
  int stm = 0;                        // 0 = sente, 1 = gote (original frame)
  std::vector<uint8_t> packedPlanes;  // binary planes (progress separate)
  float progress = 0.f;               // ply / kPlyCap
  std::vector<std::pair<int32_t, float>> policy;
  float rootQ = 0.5f;
  float a0gb = 0.5f;
  float z = 0.5f;  // game result from stm's perspective (filled at game end)
};

struct GameRecord {
  int samplingPlies = 20;
  float rootPolicyTemperature = 1.f;
  float fullSearchProb = 1.f;
  int fastSims = 32;
  size_t mateNodes = 0;  // the mate-solver settings (MctsConfig)
  int probeDepth = 0;
  size_t probeNodes = 0;
  // Mate-solver activity over the game: moves played from a proven root
  // mate, in-tree probes and the mates they proved, solver nodes.
  int rootMates = 0;
  uint64_t probes = 0, probeMates = 0, solverNodes = 0;
  std::string initialSfen;
  std::vector<std::string> moves;  // USI, original frame
  std::string finalSfen;           // position at the end (material scoring of capped games)
  // Material balance, Sente minus Gote (HaChu values, as az/material.py),
  // of every position of the game: [0] the initial one, [i] after ply i.
  std::vector<int> material;
  std::vector<Sample> samples;
  int result = 0;  // 1 = sente wins, -1 = gote wins, 0 = draw
  std::string reason;  // royals | nomoves | repetition | perpetual | plycap
};

struct SelfplayConfig {
  int sims = 200;
  int batchSize = 16;
  int tempPlies = 20;  // -1 samples throughout; otherwise sample this many plies
  // Playout cap randomization (KataGo): each move gets the full search
  // (cfg.sims, root noise) with this probability and is recorded as a
  // training sample; the other moves use a cheap unrecorded search of
  // fastSims without noise. 1 = every move full (chushogi behaviour).
  float fullSearchProb = 1.f;
  int fastSims = 32;
  uint64_t seed = 1;
  MctsConfig mcts;
};

dai::Move selectSelfplayMove(const SearchResult& search, int samplingPlies, int ply,
                             std::mt19937_64& rng);

GameRecord playGame(const std::string& initialSfen, const SelfplayConfig& cfg,
                    Evaluator& eval);

void writeGameRecord(const GameRecord& rec, const std::string& path);

// Uniform-policy / constant-value evaluator (smoke tests without a net).
struct RandomEval : Evaluator {
  void evaluate(int n, const float*, float* policyOut, float* wdlOut) override;
};

}  // namespace az
