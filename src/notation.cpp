#include "notation.h"

#include <cctype>
#include <sstream>
#include <vector>

namespace dai {

const char* const kStartSfen =
    "laknstircosigokigosicoirstknla/rc1cs1fl1btdebt1fl1cs1rc/1vo1ab1ewphlikrew1ab1vo1/"
    "rofdsmvmbidhdkfkdkdhbivmsmfdro/papapapapapapapapapapapapapapa/"
    "4gb5gb4/15/15/15/4GB5GB4/"
    "PAPAPAPAPAPAPAPAPAPAPAPAPAPAPA/ROFDSMVMBIDHDKFKDKDHBIVMSMFDRO/"
    "1VO1AB1EWKRLIPHEW1AB1VO1/RC1CS1FL1BTDEBT1FL1CS1RC/LAKNSTIRCOSIGOKIGOSICOIRSTKNLA b - 1";

namespace {

bool fail(std::string* error, const std::string& msg) {
  if (error) *error = msg;
  return false;
}

// Finds the kind whose code (uppercase, with optional "+") matches.
int kindFromCode(const std::string& code) {
  for (int k = 0; k < KindCount; ++k)
    if (info(static_cast<Kind>(k)).code == code) return k;
  return -1;
}

std::vector<std::string> split(const std::string& s) {
  std::istringstream in(s);
  std::vector<std::string> out;
  std::string t;
  while (in >> t) out.push_back(t);
  return out;
}

}  // namespace

std::string pieceCode(Piece p) {
  std::string code = info(kindOf(p)).code;
  if (colorOf(p) == Gote)
    for (char& c : code) c = static_cast<char>(std::tolower(c));
  return code;
}

std::string squareName(int sq) {
  std::string s = std::to_string(sqX(sq) + 1);
  s += static_cast<char>('a' + sqY(sq));
  return s;
}

bool parseSquare(const std::string& s, int& out) {
  size_t i = 0;
  int file = 0;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
    file = file * 10 + (s[i] - '0');
    if (file > kFiles) return false;
    ++i;
  }
  if (i == 0 || s[0] == '0' || i + 1 != s.size()) return false;
  const char r = s[i];
  if (file < 1 || r < 'a' || r >= 'a' + kRanks) return false;
  out = makeSq(file - 1, r - 'a');
  return true;
}

std::string moveToUsi(const Move& m) {
  std::string s = squareName(m.from);
  if (m.mid != kNoSquare) s += squareName(m.mid);
  s += squareName(m.to);
  if (m.promote) s += '+';
  return s;
}

bool parseUsi(const std::string& token, Move& out) {
  std::string body = token;
  bool promote = false;
  if (!body.empty() && (body.back() == '+' || body.back() == '=')) {
    promote = body.back() == '+';
    body.pop_back();
  }
  std::vector<int> squares;
  size_t i = 0;
  while (i < body.size()) {
    size_t start = i;
    while (i < body.size() && std::isdigit(static_cast<unsigned char>(body[i])))
      ++i;
    if (i == start || i >= body.size()) return false;
    int sq;
    if (!parseSquare(body.substr(start, i - start + 1), sq)) return false;
    squares.push_back(sq);
    ++i;
  }
  if (squares.size() != 2 && squares.size() != 3) return false;
  Move m;
  m.from = static_cast<int16_t>(squares[0]);
  if (squares.size() == 3) {
    m.mid = static_cast<int16_t>(squares[1]);
    m.to = static_cast<int16_t>(squares[2]);
  } else {
    m.to = static_cast<int16_t>(squares[1]);
  }
  m.promote = promote;
  out = m;
  return true;
}

bool parseSfen(const std::string& sfen, Position& out, std::string* error) {
  const std::vector<std::string> f = split(sfen);
  if (f.empty() || f.size() > 4)
    return fail(error, "expected 1-4 space-separated fields");
  Position pos;
  pos.board.fill(0);

  const std::string& b = f[0];
  int x = kFiles - 1, y = 0;
  size_t i = 0;
  while (i < b.size()) {
    const char c = b[i];
    if (c == '/') {
      if (x != -1) return fail(error, "rank " + std::to_string(y + 1) + " does not have 15 squares");
      ++y;
      x = kFiles - 1;
      if (y >= kRanks) return fail(error, "too many ranks");
      ++i;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      int n = 0;
      while (i < b.size() && std::isdigit(static_cast<unsigned char>(b[i])))
        n = n * 10 + (b[i++] - '0');
      if (n < 1 || x - n < -1) return fail(error, "bad empty-square count");
      x -= n;
      continue;
    }
    std::string code;
    if (c == '+') {
      code += '+';
      ++i;
    }
    // Exactly two letters of the same case: upper for Sente, lower for Gote.
    if (i + 1 >= b.size() || !std::isalpha(static_cast<unsigned char>(b[i])) ||
        !std::isalpha(static_cast<unsigned char>(b[i + 1])))
      return fail(error, "expected a two-letter piece code");
    const bool upper = std::isupper(static_cast<unsigned char>(b[i]));
    if (upper != static_cast<bool>(std::isupper(static_cast<unsigned char>(b[i + 1]))))
      return fail(error, "mixed-case piece code '" + b.substr(i, 2) + "'");
    for (int k = 0; k < 2; ++k)
      code += static_cast<char>(std::toupper(static_cast<unsigned char>(b[i++])));
    const int k = kindFromCode(code);
    if (k < 0) return fail(error, "unknown piece code '" + code + "'");
    if (x < 0) return fail(error, "rank " + std::to_string(y + 1) + " has more than 15 squares");
    const int sq = makeSq(x, y);
    const Color col = upper ? Sente : Gote;
    // Under the chu rule an unpromoted piece may stand where it cannot move.
    if (promotionRule() == PromotionRule::Honshogi &&
        relRank(sq, col) < info(static_cast<Kind>(k)).deadRanks)
      return fail(error, "unpromoted " + info(static_cast<Kind>(k)).name +
                             " on a rank where it cannot move (" + squareName(sq) + ")");
    pos.board[sq] = makePiece(static_cast<Kind>(k), col);
    --x;
  }
  if (y != kRanks - 1 || x != -1) return fail(error, "board must have 15 ranks of 15 squares");

  if (f.size() > 1) {
    if (f[1] == "b") pos.sideToMove = Sente;
    else if (f[1] == "w") pos.sideToMove = Gote;
    else return fail(error, "side to move must be 'b' or 'w'");
  }
  if (f.size() > 2 && f[2] != "-") {
    int sq;
    if (!parseSquare(f[2], sq)) return fail(error, "bad lion-capture square");
    pos.lionCapture = static_cast<int16_t>(sq);
  }
  if (f.size() > 3) {
    const std::string& n = f[3];
    if (n.empty() || n.size() > 9) return fail(error, "bad move number");
    for (char c : n)
      if (!std::isdigit(static_cast<unsigned char>(c))) return fail(error, "bad move number");
    pos.moveNumber = std::stoi(n);
    if (pos.moveNumber < 1) return fail(error, "bad move number");
  }
  out = pos;
  return true;
}

std::string toSfen(const Position& pos) {
  std::string s;
  for (int y = 0; y < kRanks; ++y) {
    int run = 0;
    for (int x = kFiles - 1; x >= 0; --x) {
      const Piece p = pos.board[makeSq(x, y)];
      if (!p) {
        ++run;
        continue;
      }
      if (run) s += std::to_string(run);
      run = 0;
      s += pieceCode(p);
    }
    if (run) s += std::to_string(run);
    if (y < kRanks - 1) s += '/';
  }
  s += pos.sideToMove == Sente ? " b " : " w ";
  s += pos.lionCapture == kNoSquare ? "-" : squareName(pos.lionCapture);
  s += ' ';
  s += std::to_string(pos.moveNumber);
  return s;
}

std::string diagram(const Position& pos) {
  std::string s = "  ";
  for (int x = kFiles - 1; x >= 0; --x) {
    std::string n = std::to_string(x + 1);
    s += std::string(4 - n.size(), ' ') + n;
  }
  s += '\n';
  for (int y = 0; y < kRanks; ++y) {
    s += "  ";
    for (int x = kFiles - 1; x >= 0; --x) {
      const Piece p = pos.board[makeSq(x, y)];
      std::string c = p ? pieceCode(p) : ".";
      s += std::string(4 - c.size(), ' ') + c;
    }
    s += "  ";
    s += static_cast<char>('a' + y);
    s += '\n';
  }
  s += pos.sideToMove == Sente ? "Sente (uppercase) to move" : "Gote (lowercase) to move";
  if (pos.lionCapture != kNoSquare)
    s += "; lion just captured by a non-lion on " + squareName(pos.lionCapture);
  s += '\n';
  return s;
}

}  // namespace dai
