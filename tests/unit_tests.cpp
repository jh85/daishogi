// Self-contained C++ unit tests (no reference implementation needed).
// The exhaustive rule checks are the corpus comparisons (tests/compare.py);
// these pin down notation, the initial setup, perft and a few rule examples.

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "game.h"
#include "notation.h"

using namespace dai;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++checks;                                                           \
    if (!(cond)) {                                                      \
      ++failures;                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                   \
  } while (0)

static Position sfen(const std::string& s) {
  Position p;
  std::string err;
  if (!parseSfen(s, p, &err)) {
    std::printf("bad test SFEN (%s): %s\n", err.c_str(), s.c_str());
    ++failures;
  }
  return p;
}

static std::set<std::string> legal(const Position& p) {
  std::set<std::string> out;
  for (const Move& m : generateLegal(p)) out.insert(moveToUsi(m));
  return out;
}

static std::set<std::string> movesFrom(const Position& p, const std::string& sq) {
  std::set<std::string> out;
  for (const std::string& m : legal(p))
    if (m.compare(0, sq.size(), sq) == 0) out.insert(m);  // square names end in a letter
  return out;
}

static void testNotation() {
  // Every kind round-trips through its SFEN code, for both colors.
  for (int k = 0; k < KindCount; ++k)
    for (Color c : {Sente, Gote}) {
      Position p = sfen("15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 b - 1");
      p.board[makeSq(7, 7)] = makePiece(static_cast<Kind>(k), c);
      Position q;
      CHECK(parseSfen(toSfen(p), q));
      CHECK(q.board == p.board);
    }
  // Promotion links: base <-> promoted, and promoted codes are "+" + base.
  for (int k = 0; k < KindCount; ++k) {
    const KindInfo& i = info(static_cast<Kind>(k));
    if (i.promotesTo >= 0) {
      CHECK(info(static_cast<Kind>(i.promotesTo)).base == k);
      CHECK(info(static_cast<Kind>(i.promotesTo)).code == "+" + i.code);
    }
  }
  int sq;
  CHECK(parseSquare("15o", sq) && sq == makeSq(14, 14));
  CHECK(parseSquare("1a", sq) && sq == 0);
  CHECK(!parseSquare("16a", sq));
  CHECK(!parseSquare("0a", sq));
  CHECK(!parseSquare("1p", sq));
  CHECK(squareName(makeSq(7, 14)) == "8o");
  Move m;
  CHECK(parseUsi("8m8l8k", m) && m.mid == makeSq(7, 11) && m.to == makeSq(7, 10));
  CHECK(parseUsi("15e14d+", m) && m.promote && m.from == makeSq(14, 4));
  CHECK(moveToUsi(m) == "15e14d+");
  CHECK(!parseUsi("8m", m));
  CHECK(!parseUsi("8m8l8k8j", m));
  // Malformed positions are rejected.
  Position p;
  CHECK(!parseSfen("15/15/15 b - 1", p));                       // too few ranks
  CHECK(!parseSfen("16/15/15/15/15/15/15/15/15/15/15/15/15/15/15 b - 1", p));
  CHECK(!parseSfen("PA14/15/15/15/15/15/15/15/15/15/15/15/15/15/15 b - 1", p));  // dead pawn
  CHECK(!parseSfen("15/KN14/15/15/15/15/15/15/15/15/15/15/15/15/15 b - 1", p)); // dead knight
  CHECK(!parseSfen("ZZ14/15/15/15/15/15/15/15/15/15/15/15/15/15/15 b - 1", p)); // unknown
  CHECK(!parseSfen("+KI14/15/15/15/15/15/15/15/15/15/15/15/15/15/15 b - 1", p)); // no +KI
  // Piece codes are exactly two letters of one case.
  CHECK(!parseSfen("15/G14/15/15/15/15/15/15/15/15/15/15/15/15/15 b - 1", p));   // one letter
  CHECK(!parseSfen("15/Go14/15/15/15/15/15/15/15/15/15/15/15/15/15 b - 1", p));  // mixed case
  CHECK(!parseSfen("15/N'14/15/15/15/15/15/15/15/15/15/15/15/15/15 b - 1", p));  // old style
  CHECK(parseSfen("15/GO14/15/15/15/15/15/15/15/15/15/15/15/15/15 b - 1", p) &&
        pieceCode(p.board[makeSq(14, 1)]) == "GO");
  CHECK(parseSfen("15/go14/15/15/15/15/15/15/15/15/15/15/15/15/15 b - 1", p) &&
        pieceCode(p.board[makeSq(14, 1)]) == "go");
  CHECK(!parseSfen(std::string(kStartSfen).replace(std::string(kStartSfen).find(" b "), 3, " x "), p));
}

static void testStartPosition() {
  Position p = sfen(kStartSfen);
  CHECK(toSfen(p) == kStartSfen);
  int count[2] = {0, 0};
  for (Piece x : p.board)
    if (x) ++count[colorOf(x)];
  CHECK(count[Sente] == 65 && count[Gote] == 65);
  // 180-degree symmetry with colors swapped.
  for (int s = 0; s < kSquares; ++s) {
    Piece a = p.board[s], b = p.board[kSquares - 1 - s];
    CHECK((a == 0) == (b == 0));
    if (a && b) CHECK(kindOf(a) == kindOf(b) && colorOf(a) != colorOf(b));
  }
  CHECK(kindOf(p.board[makeSq(7, 14)]) == King);
  CHECK(kindOf(p.board[makeSq(7, 12)]) == Lion);
  CHECK(kindOf(p.board[makeSq(8, 12)]) == Kirin);    // 9m, Sente's left of the lion
  CHECK(kindOf(p.board[makeSq(6, 12)]) == Phoenix);  // 7m
  CHECK(kindOf(p.board[makeSq(4, 9)]) == GoBetween); // 5j
  CHECK(kindOf(p.board[makeSq(10, 9)]) == GoBetween);  // 11j
}

static void testPerft() {
  // Values agree with haitaka-variants (perft depth 1-4 from the start).
  Position p = sfen(kStartSfen);
  CHECK(perft(p, 1) == 71);
  CHECK(perft(p, 2) == 5041);
  CHECK(perft(p, 3) == 358191);
}

static void testRules() {
  // Lion versus protected lion: adjacent capture allowed, jump forbidden.
  Position p = sfen("14ki/15/15/15/15/7go7/7li7/7LI7/15/15/15/15/15/15/KI14 b - 1");
  auto lion = movesFrom(p, "8h");
  CHECK(lion.count("8h8g"));           // adjacent: unconditional
  CHECK(lion.count("8h8g8h"));         // igui
  CHECK(lion.count("8h8g8f"));         // takes the lion, then the gold
  p = sfen("14ki/15/15/15/7go7/7li7/15/7LI7/15/15/15/15/15/15/KI14 b - 1");
  lion = movesFrom(p, "8h");
  CHECK(!lion.count("8h8f") && !lion.count("8h8g8f"));  // protected, not adjacent
  CHECK(lion.count("8h8g8h"));                          // passing is a move
  // Same, but the protector is gone.
  p = sfen("14ki/15/15/15/15/7li7/15/7LI7/15/15/15/15/15/15/KI14 b - 1");
  CHECK(legal(p).count("8h8f") && legal(p).count("8h8g8f"));
  // Tsukegui: first taking a gold makes the protected lion capturable.
  p = sfen("14ki/15/15/15/7go7/7li7/7go7/7LI7/15/15/15/15/15/15/KI14 b - 1");
  CHECK(legal(p).count("8h8g8f") && !legal(p).count("8h8f"));
  // ... but not after taking a pawn.
  p = sfen("14ki/15/15/15/7go7/7li7/7pa7/7LI7/15/15/15/15/15/15/KI14 b - 1");
  CHECK(!legal(p).count("8h8g8f") && legal(p).count("8h8g8h"));
  // A pawn taken first no longer blocks a protector (lishogi / JCSA; the
  // reference allows 8h7g6f here). The direct jump keeps the pawn in place.
  p = sfen("14ki/15/15/15/15/9li5/8pa6/7LI7/15/5bi9/15/15/15/15/KI14 b - 1");
  CHECK(!legal(p).count("8h7g6f") && legal(p).count("8h6f"));
  p = sfen("14ki/15/15/15/15/9li5/8gb6/7LI7/15/5bi9/15/15/15/15/KI14 b - 1");  // go-between
  CHECK(!legal(p).count("8h7g6f") && legal(p).count("8h6f"));
  // A lion adjacent to the start square may always be taken, also on the
  // second step (lishogi / JCSA; the reference forbids these two-step forms
  // when the lion is protected, here by the gold on 6h).
  p = sfen("14ki/15/15/15/15/15/7pa7/7LIligo5/15/15/15/15/15/15/KI14 b - 1");
  CHECK(legal(p).count("8h7h") && legal(p).count("8h8g7h") && legal(p).count("8h7g7h"));
  CHECK(!legal(p).count("8h8g6h"));  // the gold is not a lion
  // ... but adjacency to the first-step square does not count (JCSA 2004,
  // counter-example to case 4): pawn 8g, lion 8f protected by the gold 8e.
  p = sfen("14ki/15/15/15/7go7/7li7/7pa7/7LI7/15/15/15/15/15/15/KI14 b - 1");
  CHECK(!legal(p).count("8h8g8f") && !legal(p).count("8h7g8f"));
  // ... but the captured go-between still protects (JCSA 2004 case 4).
  p = sfen("14ki/15/15/15/15/7li7/7gb7/7LI7/15/15/15/15/15/15/KI14 b - 1");
  CHECK(!legal(p).count("8h8g8f") && !legal(p).count("8h8f"));
  // 先獅子: after a non-lion took a lion, a rook may not take a lion.
  p = sfen("14ki/15/15/15/15/7li7/15/15/15/15/15/7RO7/15/15/KI14 b 3c 1");
  CHECK(!legal(p).count("8l8f"));
  p.lionCapture = kNoSquare;
  CHECK(legal(p).count("8l8f"));
  // Applying a capture of a lion by a non-lion sets the state.
  p.apply(Move{static_cast<int16_t>(makeSq(7, 11)), kNoSquare, static_cast<int16_t>(makeSq(7, 5)), false});
  CHECK(p.lionCapture == makeSq(7, 5));
  // Promotion: optional on entering / leaving / inside the zone, compulsory
  // on the last rank for a pawn and the last two ranks for a knight.
  p = sfen("14ki/7PA7/12KN2/15/7SI7/15/15/15/15/15/15/15/15/15/KI14 b - 1");
  CHECK(legal(p).count("8b8a+") && !legal(p).count("8b8a"));
  CHECK(legal(p).count("3c2a+") && !legal(p).count("3c2a"));
  CHECK(legal(p).count("8e9f") && legal(p).count("8e9f+"));  // leaving
  CHECK(legal(p).count("8e8d") && legal(p).count("8e8d+"));  // inside
  // Two pawns on a file: the moving pawn may not stay unpromoted.
  p = sfen("14ki/15/12PA2/15/15/15/15/15/12PA2/15/15/15/15/15/KI14 b - 1");
  CHECK(legal(p).count("3c3b+") && !legal(p).count("3c3b") && !legal(p).count("3i3h"));
  // Violent ox: two squares, blocked by a piece in between.
  p = sfen("14ki/15/15/15/15/15/7PA7/7VO7/15/15/15/15/15/15/KI14 b - 1");
  CHECK(!legal(p).count("8h8f") && legal(p).count("8h8j") && !legal(p).count("8h8k"));
}

static void testGame() {
  // Taking the king does not end the game while the prince lives.
  Position p = sfen("14si/15/15/15/7+de6pa/15/7ki7/7LI7/15/15/15/15/15/15/KI14 b - 1");
  Game g(p);
  Move m;
  CHECK(parseUsi("8h8g", m) && g.play(m));
  CHECK(g.status().outcome == Outcome::Ongoing);
  CHECK(parseUsi("1e1f", m) && g.play(m));
  CHECK(parseUsi("8g8f8e", m) && g.play(m));
  CHECK(g.status().outcome == Outcome::SenteWins);
  CHECK(g.status().reason == EndReason::RoyalsCaptured);
  CHECK(g.legalMoves().empty());
  // Fourfold repetition draws; perpetual check loses for the checker.
  Game r(sfen(kStartSfen));
  for (int i = 0; i < 3; ++i)
    for (const char* u : {"1n1m", "15b15c", "1m1n", "15c15b"}) {
      CHECK(r.status().outcome == Outcome::Ongoing);
      CHECK(parseUsi(u, m) && r.play(m));
    }
  CHECK(r.status().outcome == Outcome::Draw && r.status().reason == EndReason::Repetition);
  Game c(sfen("10go3ki/15/15/15/15/15/15/14RO/15/15/15/15/15/15/KI14 b - 1"));
  for (int i = 0; i < 3; ++i)
    for (const char* u : {"1h1i", "5a5b", "1i1h", "5b5a"}) CHECK(parseUsi(u, m) && c.play(m));
  CHECK(c.status().outcome == Outcome::GoteWins && c.status().reason == EndReason::PerpetualCheck);
  // No legal move: the side to move loses.
  Game s(sfen("13RCKI/13PAPA/15/15/15/15/15/15/15/15/15/15/15/15/ki14 b - 1"));
  CHECK(s.status().outcome == Outcome::GoteWins && s.status().reason == EndReason::NoLegalMoves);
  // Illegal moves are refused.
  Game t(sfen(kStartSfen));
  CHECK(parseUsi("8o8n", m) && !t.play(m));
  CHECK(t.plies() == 0);
}

// ---- Side-to-move flip (180-degree rotation + color swap) ----

static std::vector<Move> sortedFlipped(std::vector<Move> moves) {
  for (Move& m : moves) m = flipMove(m);
  std::sort(moves.begin(), moves.end());
  return moves;
}

static bool samePosition(const Position& a, const Position& b) {
  return a.board == b.board && a.sideToMove == b.sideToMove &&
         a.lionCapture == b.lionCapture;
}

// Everything the rules derive from `p` corresponds in `p.flipped()`.
static bool flipConsistent(const Position& p) {
  const Position q = p.flipped();
  if (!samePosition(q.flipped(), p)) return false;
  if (q.sideToMove == p.sideToMove) return false;
  const std::vector<Move> moves = generateLegal(p);
  if (generateLegal(q) != sortedFlipped(moves)) return false;
  for (Color c : {Sente, Gote})
    if (royalAttacked(p, c) != royalAttacked(q, ~c) || p.royals(c) != q.royals(~c))
      return false;
  for (const Move& m : moves) {
    Position a = p, b = q;
    a.apply(m);
    b.apply(flipMove(m));
    if (!samePosition(a.flipped(), b)) return false;
  }
  return true;
}

static Outcome flipOutcome(Outcome o) {
  return o == Outcome::SenteWins ? Outcome::GoteWins
         : o == Outcome::GoteWins ? Outcome::SenteWins
                                  : o;
}

// Plays the same game from p and from p.flipped(); statuses must correspond
// at every ply. Returns false on any difference or illegal move.
static bool flipGameConsistent(const Position& p, const std::vector<Move>& moves) {
  Game a(p), b(p.flipped());
  auto same = [&] {
    const Status s = a.status(), t = b.status();
    return t.outcome == flipOutcome(s.outcome) && t.reason == s.reason &&
           sortedFlipped(a.legalMoves()) == b.legalMoves();
  };
  if (!same()) return false;
  for (const Move& m : moves) {
    if (!a.play(m) || !b.play(flipMove(m)) || !same()) return false;
  }
  return true;
}

static std::vector<Move> usiMoves(std::initializer_list<const char*> list, int repeat = 1) {
  std::vector<Move> out;
  for (int i = 0; i < repeat; ++i)
    for (const char* u : list) {
      Move m;
      if (!parseUsi(u, m)) ++failures;
      out.push_back(m);
    }
  return out;
}

static void testFlip() {
  // Squares: an involution through the centre square 8h.
  bool involution = true;
  for (int s = 0; s < kSquares; ++s)
    involution &= flipSquare(flipSquare(s)) == s && flipSquare(s) >= 0 &&
                  flipSquare(s) < kSquares;
  CHECK(involution);
  int sq;
  CHECK(parseSquare("8h", sq) && flipSquare(sq) == sq);
  CHECK(squareName(flipSquare(0)) == "15o");     // 1a <-> 15o
  CHECK(squareName(flipSquare(makeSq(2, 1))) == "13n");  // 3b <-> 13n
  // Pieces keep their kind and change color.
  const Piece gl = makePiece(Lion, Gote);
  CHECK(flipPiece(gl) == makePiece(Lion, Sente) && flipPiece(flipPiece(gl)) == gl);
  CHECK(flipPiece(0) == 0);
  // Moves: every square flips, promotion is kept.
  Move m;
  CHECK(parseUsi("8m8l8k", m) && moveToUsi(flipMove(m)) == "8c8d8e");
  CHECK(parseUsi("15e14d+", m) && moveToUsi(flipMove(m)) == "1k2l+");
  // The initial setup looks the same from both sides.
  const Position start = sfen(kStartSfen);
  CHECK(start.flipped().board == start.board);
  CHECK(start.flipped().sideToMove == Gote);
  CHECK(perft(start.flipped(), 3) == 358191);
  // The lion-capture square flips with the board.
  const Position lc = sfen("14ki/15/15/15/15/7li7/15/15/15/15/15/7RO7/15/15/KI14 b 3c 1");
  CHECK(lc.flipped().lionCapture == flipSquare(lc.lionCapture));
  CHECK(squareName(lc.flipped().lionCapture) == "13m");
  // Legal moves, attacks and move application correspond in rule positions
  // (lion captures, protected lions, igui, 先獅子, promotions).
  for (const char* s :
       {kStartSfen,
        "14ki/15/15/15/15/7go7/7li7/7LI7/15/15/15/15/15/15/KI14 b - 1",
        "14ki/15/15/15/7go7/7li7/7pa7/7LI7/15/15/15/15/15/15/KI14 b - 1",
        "14ki/15/15/15/15/9li5/8pa6/7LI7/15/5bi9/15/15/15/15/KI14 b - 1",
        "14ki/15/15/15/15/9li5/8gb6/7LI7/15/5bi9/15/15/15/15/KI14 b - 1",
        "14ki/15/15/15/15/15/7pa7/7LIligo5/15/15/15/15/15/15/KI14 b - 1",
        "14ki/15/15/15/15/7li7/15/15/15/15/15/7RO7/15/15/KI14 b 3c 1",
        "14si/15/15/15/7+de6pa/15/7ki7/7LI7/15/15/15/15/15/15/KI14 b - 1",
        "1vost2ki4co2knla/la1dkab6fl2strc/rc1roir9vo1/2fl+PH1+BI3kr1sm1abro/"
        "9desm3fd/10ew4/pa3papa2pa2BIpaKN1/2pa8DKPApapa/2PA6SIGB4/"
        "1VO+bi1+VM4PAPA2PAPA/PA3PA1PA1GO1VO4/1FLAB1GOEW3dh1pa1FD1/"
        "RC2CSCORO8RC/LA1SMSTIRSI1BT2FLKR1ABLA/2FD4KIBT6 b 12c 1"}) {
    const Position p = sfen(s);
    CHECK(flipConsistent(p));
    Position w = p;
    w.sideToMove = ~w.sideToMove;
    w.lionCapture = kNoSquare;
    CHECK(flipConsistent(w));
  }
  // Random games (with a capture bias, so that lions, promotions and
  // endings occur): every position and every game status corresponds.
  uint64_t rng = 12345;
  auto next = [&] {
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<uint32_t>(rng >> 33);
  };
  for (int game = 0; game < 6; ++game) {
    Game g(start);
    bool positionsOk = true;
    while (g.status().outcome == Outcome::Ongoing && g.plies() < 600) {
      positionsOk &= flipConsistent(g.current());
      std::vector<Move> moves = g.legalMoves();
      std::vector<Move> caps;
      for (const Move& mv : moves)  // (a lion's pass ends on its own square)
        if ((mv.to != mv.from && g.current().board[mv.to]) ||
            (mv.mid != kNoSquare && g.current().board[mv.mid]))
          caps.push_back(mv);
      const std::vector<Move>& pool = (!caps.empty() && next() % 4 != 0) ? caps : moves;
      g.playUnchecked(pool[next() % pool.size()]);
    }
    positionsOk &= flipConsistent(g.current());
    CHECK(positionsOk);
    CHECK(flipGameConsistent(start, g.moves()));
  }
  // Game endings in both orientations: prince after king, repetition,
  // perpetual check, no legal move.
  CHECK(flipGameConsistent(
      sfen("14si/15/15/15/7+de6pa/15/7ki7/7LI7/15/15/15/15/15/15/KI14 b - 1"),
      usiMoves({"8h8g", "1e1f", "8g8f8e"})));
  CHECK(flipGameConsistent(start, usiMoves({"1n1m", "15b15c", "1m1n", "15c15b"}, 3)));
  CHECK(flipGameConsistent(
      sfen("10go3ki/15/15/15/15/15/15/14RO/15/15/15/15/15/15/KI14 b - 1"),
      usiMoves({"1h1i", "5a5b", "1i1h", "5b5a"}, 3)));
  CHECK(flipGameConsistent(
      sfen("13RCKI/13PAPA/15/15/15/15/15/15/15/15/15/15/15/15/ki14 b - 1"), {}));
}

int main() {
  testNotation();
  testStartPosition();
  testPerft();
  testRules();
  testGame();
  testFlip();
  if (failures) {
    std::printf("%d of %d unit checks FAILED\n", failures, checks);
    return 1;
  }
  std::printf("OK: %d unit checks passed\n", checks);
  return 0;
}
