// Ported from ../chushogi (src/az/selfplay.cpp, commit d578c81).

#include "selfplay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <fstream>
#include <random>

#include "../game.h"
#include "../material.h"
#include "../notation.h"

namespace az {

namespace {

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const std::vector<uint8_t>& data) {
  std::string out;
  for (size_t i = 0; i < data.size(); i += 3) {
    uint32_t v = data[i] << 16;
    if (i + 1 < data.size()) v |= data[i + 1] << 8;
    if (i + 2 < data.size()) v |= data[i + 2];
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
    out += i + 1 < data.size() ? kB64[(v >> 6) & 63] : '=';
    out += i + 2 < data.size() ? kB64[v & 63] : '=';
  }
  return out;
}

std::string jsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

// "win sente royals" -> "royals", "draw repetition" -> "repetition".
std::string reasonOf(const dai::Status& st) {
  const std::string token = dai::statusToken(st);
  return token.substr(token.rfind(' ') + 1);
}

}  // namespace

void RandomEval::evaluate(int n, const float*, float* policyOut, float* wdlOut) {
  std::fill(policyOut, policyOut + size_t(n) * kPolicySize, 0.0f);
  for (int i = 0; i < n; ++i) {
    wdlOut[3 * i] = 1.f / 3;
    wdlOut[3 * i + 1] = 1.f / 3;
    wdlOut[3 * i + 2] = 1.f / 3;
  }
}

dai::Move selectSelfplayMove(const SearchResult& search, int samplingPlies, int ply,
                             std::mt19937_64& rng, float temperature) {
  if (samplingPlies >= 0 && ply >= samplingPlies) return search.bestMove;
  std::uniform_real_distribution<float> uni(0.f, 1.f);
  const float pick = uni(rng);
  if (temperature != 1.f && !search.policy.empty()) {
    // visits^(1/T), scaled by the largest share so the powers cannot underflow.
    float top = 0.f;
    for (const auto& kv : search.policy) top = std::max(top, kv.second);
    std::vector<double> w;
    w.reserve(search.policy.size());
    double total = 0.0;
    for (const auto& kv : search.policy) {
      w.push_back(kv.second > 0.f ? std::pow(double(kv.second) / top, 1.0 / temperature) : 0.0);
      total += w.back();
    }
    double acc = 0.0;
    for (size_t i = 0; i < w.size(); ++i) {
      acc += w[i] / total;
      if (pick <= acc + 1e-9) return indexToMove(search.policy[i].first);
    }
    return search.bestMove;
  }
  float acc = 0.f;
  for (const auto& kv : search.policy) {
    acc += kv.second;
    if (pick <= acc + 1e-9f) return indexToMove(kv.first);
  }
  return search.bestMove;
}

float moveTemperature(const SelfplayConfig& cfg, int ply) {
  if (cfg.moveTempEarly == cfg.moveTempLate) return cfg.moveTempLate;
  return cfg.moveTempLate + (cfg.moveTempEarly - cfg.moveTempLate) *
                                std::pow(0.5f, static_cast<float>(ply) / cfg.moveTempHalflife);
}

GameRecord playGame(const std::string& initialSfen, const SelfplayConfig& cfg,
                    Evaluator& eval) {
  dai::Position start;
  if (!dai::parseSfen(initialSfen, start)) {
    std::fprintf(stderr, "error: bad initial SFEN\n");
    return {};
  }
  std::mt19937_64 rng(cfg.seed);

  GameRecord rec;
  rec.initialSfen = initialSfen;
  rec.samplingPlies = cfg.tempPlies;
  rec.moveTempEarly = cfg.moveTempEarly;
  rec.moveTempLate = cfg.moveTempLate;
  rec.moveTempHalflife = cfg.moveTempHalflife;
  rec.materialBlend = cfg.mcts.materialBlend;
  rec.materialScale = cfg.mcts.materialScale;
  rec.capturePrior = cfg.mcts.capturePrior;
  rec.rootPolicyTemperature = cfg.mcts.rootPolicyTemperature;
  rec.fullSearchProb = cfg.fullSearchProb;
  rec.fastSims = cfg.fastSims;
  rec.mateNodes = cfg.mcts.rootMateNodes;
  rec.probeDepth = cfg.mcts.probeDepth;
  rec.probeNodes = cfg.mcts.probeNodes;

  dai::Game game(start);
  std::vector<uint64_t> hashes;  // gameKey() of the positions BEFORE now
  Mcts mcts(cfg.mcts);
  MctsConfig fastCfg = cfg.mcts;
  fastCfg.sims = std::max(1, cfg.fastSims);
  fastCfg.rootNoise = false;
  Mcts fast(fastCfg);
  std::uniform_real_distribution<float> coin(0.f, 1.f);
  std::vector<float> planes(kNumPlanes * kBoard);

  while (true) {
    const dai::Position& pos = game.current();
    const int plies = static_cast<int>(game.plies());
    rec.material.push_back(dai::materialBalance(pos));  // src/material.h
    // Adjudication: the game rules, then the ply cap.
    const dai::Status st = game.status();
    if (st.outcome != dai::Outcome::Ongoing) {
      rec.reason = reasonOf(st);
      rec.result = st.outcome == dai::Outcome::SenteWins  ? 1
                   : st.outcome == dai::Outcome::GoteWins ? -1
                                                          : 0;
      break;
    }
    if (plies >= cfg.mcts.maxPlyCap) {
      rec.reason = "plycap";
      rec.result = 0;
      break;
    }

    const bool flip = pos.sideToMove == dai::Gote;
    const dai::Position canon = flip ? pos.flipped() : pos;
    const uint64_t curKey = gameKey(pos);
    const int repCount =  // occurrences before the current position
        static_cast<int>(std::count(hashes.begin(), hashes.end(), curKey));
    // The coin is drawn only when randomization is on, so the default
    // keeps the chushogi RNG stream.
    const bool full = cfg.fullSearchProb >= 1.f || coin(rng) < cfg.fullSearchProb;
    SearchResult res = (full ? mcts : fast).search(canon, flip, hashes, plies, eval, rng);
    rec.rootMates += res.provenMate;
    rec.probes += res.probes;
    rec.probeMates += res.probeMates;
    rec.solverNodes += res.mateNodes;
    if (res.policy.empty()) {  // no legal moves (should be caught by status)
      rec.reason = "nomoves";
      rec.result = flip ? 1 : -1;
      break;
    }

    // Record the sample (full searches only).
    if (full) {
      Sample smp;
      smp.ply = plies;
      smp.stm = pos.sideToMove;
      encodePlanes(canon, repCount, plies, planes.data());
      smp.packedPlanes.resize(kPackedBytes);
      smp.progress = packPlanes(planes.data(), smp.packedPlanes.data());
      smp.policy = res.policy;
      smp.rootQ = res.rootQ;
      smp.a0gb = res.a0gb;
      rec.samples.push_back(std::move(smp));
    }

    // Choose the move: temperature sampling early, argmax later.
    const dai::Move chosen =
        selectSelfplayMove(res, cfg.tempPlies, plies, rng, moveTemperature(cfg, plies));
    const dai::Move real = flip ? dai::flipMove(chosen) : chosen;
    rec.moves.push_back(dai::moveToUsi(real));
    hashes.push_back(curKey);
    game.playUnchecked(real);
  }

  rec.finalSfen = dai::toSfen(game.current());

  // Fill z targets.
  for (Sample& s : rec.samples) {
    if (rec.result == 0)
      s.z = 0.5f;
    else
      s.z = (s.stm == 0 && rec.result == 1) || (s.stm == 1 && rec.result == -1) ? 1.0f
                                                                                : 0.0f;
  }
  return rec;
}

void writeGameRecord(const GameRecord& rec, const std::string& path) {
  std::ofstream f(path);
  f << "{\"selfplay\":{\"sampling_plies\":" << rec.samplingPlies
    << ",\"move_temperature\":{\"early\":" << rec.moveTempEarly
    << ",\"late\":" << rec.moveTempLate << ",\"halflife\":" << rec.moveTempHalflife << "}"
    << ",\"material_blend\":{\"lambda\":" << rec.materialBlend
    << ",\"scale\":" << rec.materialScale << "},\"capture_prior\":" << rec.capturePrior
    << ",\"root_policy_temperature\":" << rec.rootPolicyTemperature
    << ",\"full_search_prob\":" << rec.fullSearchProb << ",\"fast_sims\":" << rec.fastSims
    << ",\"mate_nodes\":" << rec.mateNodes << ",\"probe_depth\":" << rec.probeDepth
    << ",\"probe_nodes\":" << rec.probeNodes << ",\"counter_strike\":\""
    << dai::counterStrikeName(dai::counterStrike()) << "\",\"promotion\":\""
    << dai::promotionRuleName(dai::promotionRule()) << "\"},\n";
  f << " \"mate_stats\": {\"root_mates\":" << rec.rootMates << ",\"probes\":" << rec.probes
    << ",\"probe_mates\":" << rec.probeMates << ",\"solver_nodes\":" << rec.solverNodes
    << "},\n";
  f << " \"initial\": \"" << jsonEscape(rec.initialSfen) << "\",\n";
  f << " \"final\": \"" << jsonEscape(rec.finalSfen) << "\",\n";
  f << " \"result\": " << rec.result << ", \"reason\": \"" << jsonEscape(rec.reason)
    << "\",\n \"moves\": [";
  for (size_t i = 0; i < rec.moves.size(); ++i)
    f << (i ? ", " : "") << "\"" << rec.moves[i] << "\"";
  f << "],\n \"material\": [";
  for (size_t i = 0; i < rec.material.size(); ++i) f << (i ? ", " : "") << rec.material[i];
  f << "],\n \"samples\": [\n";
  for (size_t i = 0; i < rec.samples.size(); ++i) {
    const Sample& s = rec.samples[i];
    f << "  {\"ply\": " << s.ply << ", \"stm\": " << s.stm << ", \"policy\": [";
    for (size_t j = 0; j < s.policy.size(); ++j)
      f << (j ? ", " : "") << "[" << s.policy[j].first << ", " << s.policy[j].second
        << "]";
    f << "], \"rootq\": " << s.rootQ << ", \"a0gb\": " << s.a0gb << ", \"z\": " << s.z
      << ", \"progress\": " << s.progress << ", \"planes\": \""
      << base64(s.packedPlanes) << "\"}" << (i + 1 < rec.samples.size() ? ",\n" : "\n");
  }
  f << "]}\n";
}

}  // namespace az
