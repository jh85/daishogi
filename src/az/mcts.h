// AlphaZero-style MCTS (PUCT) for dai shogi.
//
// Ported from ../chushogi (src/az/mcts.h, commit d578c81), including the
// mate-solver probes (src/mate.h): the root mate override and the shallow
// probe of every new tree node. Left out: proof-aware root selection, the
// USI-mode repetition guard for mate lines (a simpler PV check is used),
// and the experimental safe-sampling / safe-policy options.
//
// dlshogi/JHBR3 conventions: c_init + log-scaled c_base, FPU for unvisited
// edges (parent Q minus a reduction over the visited policy mass), virtual
// loss for batched leaf collection, root Dirichlet noise for self-play.
//
// Values are win probabilities in [0, 1] from the perspective of the side to
// move at each node. Edge statistics (n, w) live in the parent's perspective.
//
// Per root search, search() returns the visit-count policy plus the three
// value targets used by the trainer: z (filled in by the game loop), rootQ
// (soft-Z: the root's mean backed-up value) and the A0GB target (value at the
// end of the greedy max-visit path — the leaf's net eval, or the true outcome
// when the path reaches a terminal).
//
// In-tree terminals: royal capture and no legal move (exact), a position
// where the side to move can capture the opponent's last royal (an exact
// win in one) or, with probeDepth, force such a capture with checks (a
// mate the solver proves), the game ply cap, and repetition, scored as a draw (by default on the first recurrence,
// like chushogi's early cutoff). Perpetual check is not distinguished from
// repetition inside the tree; the game loop adjudicates it exactly.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <random>
#include <vector>

#include <memory>

#include "../mate.h"
#include "../position.h"
#include "encode.h"

namespace az {

struct Evaluator {
  virtual ~Evaluator() = default;
  // Batch-evaluate canonical (side-to-move frame) positions.
  // planesIn: n * kNumPlanes * kBoard floats.
  // policyOut: n * kPolicySize logits; wdlOut: n * 3 floats.
  virtual void evaluate(int n, const float* planesIn, float* policyOut,
                        float* wdlOut) = 0;
  // Only the requested canonical policy entries need to be populated.
  virtual void evaluateLegal(int n, const float* x,
                             const std::vector<std::vector<int>>& indices, float* p,
                             float* v) {
    (void)indices;
    evaluate(n, x, p, v);
  }
};

struct MctsConfig {
  int sims = 200;
  int batchSize = 16;
  float cInit = 1.25f;
  float cBase = 19652.f;
  float cInitRoot = 1.25f;
  float cBaseRoot = 19652.f;
  float fpuReduction = 0.27f;
  float fpuReductionRoot = 0.0f;
  float dirichletEps = 0.25f;
  float dirichletAlpha = 0.15f;
  float rootPolicyTemperature = 1.f;  // softmax(logits / T), root only, before noise
  bool rootNoise = true;              // self-play on; eval matches off
  int maxPlyCap = kPlyCap;            // game draw cap (plies from game start)
  int repetitionOccurrences = 2;      // 2 = early cutoff; 4 = actual fourfold
  bool collectStats = false;          // diagnostic root move statistics
  // bestMove skips a root move after which the opponent can capture the
  // mover's last royal, unless every move allows that. Visits and the
  // policy target are unchanged. Match play only.
  bool rootRoyalFilter = false;
  // Mate solver (src/mate.h; chushogi's defaults are 5000 / 3 / 2000, set
  // by run_loop.py). rootMateNodes > 0: before searching, the root is solved
  // with this node budget for mates up to rootMatePly plies, cut so that the
  // line ends before the game cap; a proven mate is played at once with a
  // one-hot policy and won values. probeDepth > 0: every new tree node whose
  // mate would end before the game cap is probed with maxPly probeDepth and
  // probeNodes; a proven mate makes it a terminal win for its side to move.
  // Repetition is not modelled inside mate lines (the root PV must not
  // reach a fourth occurrence).
  size_t rootMateNodes = 0;
  int rootMatePly = 200;
  int probeDepth = 0;
  size_t probeNodes = 2000;
  // Ends the search early; checked between batches, after the first one.
  const std::atomic<bool>* stop = nullptr;
};

struct Edge {
  dai::Move move;
  int32_t policyIndex = -1;
  int32_t child = -1;  // arena index; -1 until first expansion
  float prior = 0.f;
  int32_t n = 0;
  float w = 0.f;  // backed-up value sum, parent (mover) perspective
  int32_t inflight = 0;
  int32_t repetitionVisits = 0;
};

struct Node {
  std::vector<Edge> edges;
  int8_t terminal = -1;    // -1 none; 0 stm loses; 1 stm wins; 2 draw
  bool evaluated = false;  // leafValue/edges filled
  float leafValue = 0.5f;  // net eval (or terminal value), stm perspective
  std::array<float, 3> wdl{{0.f, 1.f, 0.f}};
  int32_t visits = 0;  // descents through this node
  bool repetitionDraw = false;
  int priorOccurrences = 0;
};

struct RootMoveStats {
  dai::Move move;
  int visits = 0;
  float prior = 0.f;
  float q = 0.5f;
  int repetitionVisits = 0;
  bool directRepetitionDraw = false;
  int childPriorOccurrences = 0;
};

struct SearchResult {
  dai::Move bestMove;
  std::vector<std::pair<int, float>> policy;  // policy index, visit prob
  float rootQ = 0.5f;
  float a0gb = 0.5f;
  int32_t rootVisits = 0;
  // Root royal filter: moves with more visits than bestMove that it skipped
  // (the first is mostVisited), and whether every move was hanging.
  int royalFilterSkipped = 0;
  bool royalFilterAllHang = false;
  // The root side could capture the opponent's last royal: bestMove is that
  // capture, the policy is one-hot and no search ran.
  bool immediateWin = false;
  // Root moves with visits whose result is a proven loss, removed from the
  // policy target and from move choice.
  int provenLossesPruned = 0;
  // The root mate solver proved a mate: bestMove is its first move, the
  // policy is one-hot and no search ran. mateLength = the PV's plies.
  bool provenMate = false;
  int mateLength = 0;
  // Solver work in this search: root solve and in-tree probes.
  uint64_t mateNodes = 0;
  int probes = 0, probeMates = 0;
  dai::Move mostVisited;
  float rootValue = 0.5f;
  std::array<float, 3> rootWdl{{0.f, 1.f, 0.f}};
  std::vector<RootMoveStats> rootMoves;
};

class Mcts {
 public:
  explicit Mcts(MctsConfig cfg = {});

  // Runs cfg.sims playouts from `root` (canonical frame: Sente to move).
  // `rootFlipped` = the real game position is root.flipped() (Gote to
  // move). `gameHashes` = gameKey() of all earlier positions of the real
  // game (repetition detection); `gamePly` = plies already played.
  SearchResult search(const dai::Position& root, bool rootFlipped,
                      const std::vector<uint64_t>& gameHashes, int gamePly,
                      Evaluator& eval, std::mt19937_64& rng);

 private:
  struct Descent {
    std::vector<std::pair<int, int>> path;  // (node, edge) from root
    dai::Position pos;                      // position at the leaf
    std::vector<uint64_t> hashPath;         // in-tree position keys
    int leaf = -1;                          // arena index of the leaf
    bool needsEval = false;
    float value = 0.5f;  // terminal value if !needsEval
  };

  int newNode();
  void fillNode(int nodeIdx, const dai::Position& pos,
                const std::vector<dai::Move>& legal, const float* policy,
                const float* wdl);
  Descent descend();
  void backup(const Descent& d);
  void runBatch(Evaluator& eval);
  int pickChild(int nodeIdx, bool isRoot) const;
  // gameKey() of a tree position (root frame) in the real game's frame.
  uint64_t realKey(const dai::Position& pos) const;

  dai::MateSolver& mateSolver();
  // A proven root mate whose PV reaches no fourth occurrence of a position.
  bool rootMate(dai::MateAnswer& out);

  MctsConfig cfg_;
  std::unique_ptr<dai::MateSolver> mate_;  // reused across probes
  uint64_t mateNodes_ = 0;
  int probes_ = 0, probeMates_ = 0;
  std::deque<Node> arena_;
  int root_ = -1;
  dai::Position rootPos_;
  bool rootFlipped_ = false;
  uint64_t rootKey_ = 0;
  std::vector<uint64_t> gameHashes_;
  int gamePly_ = 0;
};

// Repetition key of a position in the real game's frame. It includes the
// side to move, so a position and its flipped twin never collide (they do
// after a lion pass in the symmetric initial setup). Equal keys = same
// state for the repetition rule (dai::Position::sameState).
inline uint64_t gameKey(const dai::Position& pos) { return pos.key(); }

// Position-only game end (royal capture, no legal move), from the side to
// move's view: -1 ongoing, 0 the side to move has lost, 1 it has won.
int positionResult(const dai::Position& pos);

// A legal move (from `legal`, the side to move's legal moves) that captures
// the opponent's last royal, i.e. wins the game at once. The search treats
// such positions as proven wins and plays such a root move directly: with a
// learned prior and the in-tree FPU reduction, a single capturing reply
// among ~100 is otherwise easily never visited, and the search walks into
// king captures (see az/README.md, run2).
bool findRoyalCapture(const dai::Position& pos, const std::vector<dai::Move>& legal,
                      dai::Move* out = nullptr);

// True if after `m` the opponent has a legal reply that captures the mover's
// last royal. False if the game already ended, the mover has no royal or `m`
// itself captures the opponent's last royal.
bool hangsLastRoyal(const dai::Position& pos, const dai::Move& m);

}  // namespace az
