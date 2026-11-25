// Ported from ../chushogi (src/az/selfplay.cpp, commit d578c81).

#include "selfplay.h"

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <fstream>
#include <random>

#include "../game.h"
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

// HaChu's dai shogi piece values, the table az/material.py uses (see its
// docstring); tests/az/selfplay_test.py checks that both agree.
int materialValue(dai::Kind k) {
  static const std::array<int, dai::KindCount> table = [] {
    const std::map<std::string, int> v = {
        {"PA", 40}, {"LA", 150}, {"KN", 60}, {"SI", 100}, {"GO", 151}, {"BI", 250},
        {"RO", 300}, {"KI", 280}, {"DH", 350}, {"DK", 400}, {"LI", 1000}, {"DE", 201},
        {"CO", 100}, {"FL", 150}, {"RC", 150}, {"BT", 152}, {"KR", 154}, {"PH", 153},
        {"GB", 50}, {"SM", 200}, {"VM", 200}, {"FK", 600}, {"IR", 80}, {"ST", 50},
        {"EW", 80}, {"AB", 60}, {"CS", 70}, {"VO", 200}, {"FD", 150},
        // promoted kinds: the value of the piece they move as
        {"+PA", 151}, {"+KN", 151}, {"+IR", 151}, {"+ST", 151}, {"+EW", 151},
        {"+AB", 151}, {"+CS", 151}, {"+VO", 151}, {"+FD", 151}, {"+LA", 350},
        {"+SI", 200}, {"+GO", 300}, {"+BI", 350}, {"+RO", 400}, {"+DH", 500},
        {"+DK", 550}, {"+DE", 270}, {"+CO", 200}, {"+FL", 250}, {"+RC", 250},
        {"+BT", 300}, {"+KR", 1000}, {"+PH", 600}, {"+GB", 201}, {"+SM", 400},
        {"+VM", 400}};
    std::array<int, dai::KindCount> t{};
    for (int k = 0; k < dai::KindCount; ++k) {
      const auto it = v.find(dai::info(static_cast<dai::Kind>(k)).code);
      if (it == v.end())
        throw std::logic_error("no material value for " + dai::info(static_cast<dai::Kind>(k)).code);
      t[k] = it->second;
    }
    return t;
  }();
  return table[k];
}

// Sente's material minus Gote's.
int materialBalance(const dai::Position& pos) {
  int b = 0;
  for (const dai::Piece p : pos.board)
    if (p) b += (dai::colorOf(p) == dai::Sente ? 1 : -1) * materialValue(dai::kindOf(p));
  return b;
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
                             std::mt19937_64& rng) {
  if (samplingPlies >= 0 && ply >= samplingPlies) return search.bestMove;
  std::uniform_real_distribution<float> uni(0.f, 1.f);
  const float pick = uni(rng);
  float acc = 0.f;
  for (const auto& kv : search.policy) {
    acc += kv.second;
    if (pick <= acc + 1e-9f) return indexToMove(kv.first);
  }
  return search.bestMove;
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
    rec.material.push_back(materialBalance(pos));
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
    const dai::Move chosen = selectSelfplayMove(res, cfg.tempPlies, plies, rng);
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
