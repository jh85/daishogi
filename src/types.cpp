#include "types.h"

namespace dai {

namespace {

// Direction atoms (Sente POV).
const std::vector<Dir> kKingSteps = {{1, -1}, {0, -1}, {-1, -1}, {1, 0},
                                     {-1, 0}, {1, 1},  {0, 1},  {-1, 1}};
const std::vector<Dir> kGoldSteps = {{1, -1}, {0, -1}, {-1, -1},
                                     {1, 0},  {-1, 0}, {0, 1}};
const std::vector<Dir> kOrthSteps = {{0, -1}, {1, 0}, {-1, 0}, {0, 1}};
const std::vector<Dir> kDiagSteps = {{1, -1}, {-1, -1}, {1, 1}, {-1, 1}};
const std::vector<Dir> kElephantSteps = {{1, -1}, {0, -1}, {-1, -1}, {1, 0},
                                         {-1, 0}, {1, 1},  {-1, 1}};

std::vector<Ray> rays(std::initializer_list<Dir> dirs, int limit = 0) {
  std::vector<Ray> r;
  for (Dir d : dirs) r.push_back({d.dx, d.dy, static_cast<int8_t>(limit)});
  return r;
}
const std::vector<Ray> kOrthRays = rays({{0, -1}, {1, 0}, {-1, 0}, {0, 1}});
const std::vector<Ray> kDiagRays = rays({{1, -1}, {-1, -1}, {1, 1}, {-1, 1}});
const std::vector<Ray> kAllRays = rays({{1, -1}, {0, -1}, {-1, -1}, {1, 0},
                                        {-1, 0}, {1, 1},  {0, 1},  {-1, 1}});

std::vector<Dir> lionSteps() {
  std::vector<Dir> s;
  for (int dy = -2; dy <= 2; ++dy)
    for (int dx = 2; dx >= -2; --dx)
      if (dx || dy) s.push_back({static_cast<int8_t>(dx), static_cast<int8_t>(dy)});
  return s;
}

std::array<KindInfo, KindCount> buildTable() {
  std::array<KindInfo, KindCount> t;
  auto def = [&](Kind k, const char* code, const char* name, const char* kanji,
                 const char* refId, std::vector<Dir> steps,
                 std::vector<Ray> slides, int promo) {
    KindInfo& i = t[k];
    i.code = code;
    i.name = name;
    i.kanji = kanji;
    i.refId = refId;
    i.steps = std::move(steps);
    i.rays = std::move(slides);
    i.promotesTo = promo;
  };
  // ---- unpromoted kinds ----
  def(Pawn, "PA", "Pawn", "歩兵", "aa", {{0, -1}}, {}, Tokin);
  def(Lance, "LA", "Lance", "香車", "ab", {}, rays({{0, -1}}), WhiteHorse);
  def(Knight, "KN", "Knight", "桂馬", "ac", {{1, -2}, {-1, -2}}, {}, GoldN);
  def(Silver, "SI", "Silver General", "銀将", "ad",
      {{1, -1}, {0, -1}, {-1, -1}, {1, 1}, {-1, 1}}, {}, VerticalMoverS);
  def(Gold, "GO", "Gold General", "金将", "ae", kGoldSteps, {}, RookG);
  def(Bishop, "BI", "Bishop", "角行", "af", {}, kDiagRays, HorseB);
  def(Rook, "RO", "Rook", "飛車", "ag", {}, kOrthRays, DragonR);
  def(King, "KI", "King", "玉将", "ah", kKingSteps, {}, -1);
  def(DragonHorse, "DH", "Dragon Horse", "龍馬", "am", kOrthSteps, kDiagRays,
      HornedFalcon);
  def(DragonKing, "DK", "Dragon King", "龍王", "an", kDiagSteps, kOrthRays,
      SoaringEagle);
  def(Lion, "LI", "Lion", "獅子", "bl", lionSteps(), {}, -1);
  def(DrunkElephant, "DE", "Drunk Elephant", "醉象", "cf", kElephantSteps, {},
      Prince);
  def(Copper, "CO", "Copper General", "銅将", "ci",
      {{1, -1}, {0, -1}, {-1, -1}, {0, 1}}, {}, SideMoverC);
  def(Leopard, "FL", "Ferocious Leopard", "猛豹", "cl",
      {{1, -1}, {0, -1}, {-1, -1}, {1, 1}, {0, 1}, {-1, 1}}, {}, BishopF);
  def(ReverseChariot, "RC", "Reverse Chariot", "反車", "cm", {},
      rays({{0, -1}, {0, 1}}), Whale);
  def(BlindTiger, "BT", "Blind Tiger", "盲虎", "cn",
      {{1, -1}, {-1, -1}, {1, 0}, {-1, 0}, {1, 1}, {0, 1}, {-1, 1}}, {},
      FlyingStag);
  def(Kirin, "KR", "Kirin", "麒麟", "co",
      {{0, -2}, {1, -1}, {-1, -1}, {2, 0}, {-2, 0}, {1, 1}, {-1, 1}, {0, 2}},
      {}, LionO);
  def(Phoenix, "PH", "Phoenix", "鳳凰", "cp",
      {{2, -2}, {-2, -2}, {0, -1}, {1, 0}, {-1, 0}, {0, 1}, {2, 2}, {-2, 2}},
      {}, FreeKingX);
  def(GoBetween, "GB", "Go-Between", "仲人", "cq", {{0, -1}, {0, 1}}, {},
      ElephantI);
  def(SideMover, "SM", "Side Mover", "横行", "cr", {{0, -1}, {0, 1}},
      rays({{1, 0}, {-1, 0}}), FreeBoar);
  def(VerticalMover, "VM", "Vertical Mover", "竪行", "cs", {{1, 0}, {-1, 0}},
      rays({{0, -1}, {0, 1}}), FlyingOx);
  def(FreeKing, "FK", "Free King", "奔王", "ct", {}, kAllRays, -1);
  def(Iron, "IR", "Iron General", "鉄将", "dp", {{1, -1}, {0, -1}, {-1, -1}},
      {}, GoldI);
  def(Stone, "ST", "Stone General", "石将", "dq", {{1, -1}, {-1, -1}}, {},
      GoldSt);
  def(EvilWolf, "EW", "Evil Wolf", "悪狼", "dr",
      {{1, -1}, {0, -1}, {-1, -1}, {1, 0}, {-1, 0}}, {}, GoldEW);
  def(AngryBoar, "AB", "Angry Boar", "嗔猪", "ds", kOrthSteps, {}, GoldAB);
  def(CatSword, "CS", "Cat Sword", "猫刄", "dt", kDiagSteps, {}, GoldCS);
  def(ViolentOx, "VO", "Violent Ox", "猛牛", "du", {},
      rays({{0, -1}, {1, 0}, {-1, 0}, {0, 1}}, 2), GoldVO);
  def(FlyingDragon, "FD", "Flying Dragon", "飛龍", "dv", {},
      rays({{1, -1}, {-1, -1}, {1, 1}, {-1, 1}}, 2), GoldFD);
  // ---- promoted kinds ----
  def(Tokin, "+PA", "Tokin (gold)", "と金", "ai", kGoldSteps, {}, -1);
  def(WhiteHorse, "+LA", "White Horse", "白駒", "cu", {},
      rays({{1, -1}, {0, -1}, {-1, -1}, {0, 1}}), -1);
  def(GoldN, "+KN", "Gold (from Knight)", "成桂", "do", kGoldSteps, {}, -1);
  def(GoldI, "+IR", "Gold (from Iron)", "成鉄", "do", kGoldSteps, {}, -1);
  def(GoldSt, "+ST", "Gold (from Stone)", "成石", "do", kGoldSteps, {}, -1);
  def(GoldEW, "+EW", "Gold (from Evil Wolf)", "成狼", "do", kGoldSteps, {}, -1);
  def(GoldAB, "+AB", "Gold (from Angry Boar)", "成猪", "do", kGoldSteps, {}, -1);
  def(GoldCS, "+CS", "Gold (from Cat Sword)", "成猫", "do", kGoldSteps, {}, -1);
  def(GoldVO, "+VO", "Gold (from Violent Ox)", "成牛", "do", kGoldSteps, {}, -1);
  def(GoldFD, "+FD", "Gold (from Flying Dragon)", "成龍", "do", kGoldSteps, {},
      -1);
  def(VerticalMoverS, "+SI", "Vertical Mover (from Silver)", "竪行", "de",
      {{1, 0}, {-1, 0}}, rays({{0, -1}, {0, 1}}), -1);
  def(RookG, "+GO", "Rook (from Gold)", "飛車", "dd", {}, kOrthRays, -1);
  def(HorseB, "+BI", "Dragon Horse (from Bishop)", "龍馬", "dl", kOrthSteps,
      kDiagRays, -1);
  def(DragonR, "+RO", "Dragon King (from Rook)", "龍王", "dm", kDiagSteps,
      kOrthRays, -1);
  def(HornedFalcon, "+DH", "Horned Falcon", "角鷹", "cz", {{0, -2}, {0, -1}},
      rays({{1, -1}, {-1, -1}, {1, 0}, {-1, 0}, {1, 1}, {0, 1}, {-1, 1}}), -1);
  def(SoaringEagle, "+DK", "Soaring Eagle", "飛鷲", "db",
      {{2, -2}, {-2, -2}, {1, -1}, {-1, -1}},
      rays({{0, -1}, {1, 0}, {-1, 0}, {1, 1}, {0, 1}, {-1, 1}}), -1);
  def(Prince, "+DE", "Prince", "太子", "cg", kKingSteps, {}, -1);
  def(SideMoverC, "+CO", "Side Mover (from Copper)", "横行", "df",
      {{0, -1}, {0, 1}}, rays({{1, 0}, {-1, 0}}), -1);
  def(BishopF, "+FL", "Bishop (from Leopard)", "角行", "dg", {}, kDiagRays, -1);
  def(Whale, "+RC", "Whale", "鯨鯢", "cv", {},
      rays({{0, -1}, {1, 1}, {0, 1}, {-1, 1}}), -1);
  def(FlyingStag, "+BT", "Flying Stag", "飛鹿", "cw",
      {{1, -1}, {-1, -1}, {1, 0}, {-1, 0}, {1, 1}, {-1, 1}},
      rays({{0, -1}, {0, 1}}), -1);
  def(LionO, "+KR", "Lion (from Kirin)", "獅子", "dh", lionSteps(), {}, -1);
  def(FreeKingX, "+PH", "Free King (from Phoenix)", "奔王", "dj", {}, kAllRays,
      -1);
  def(ElephantI, "+GB", "Drunk Elephant (from Go-Between)", "醉象", "dk",
      kElephantSteps, {}, -1);
  def(FreeBoar, "+SM", "Free Boar", "奔猪", "cx", {},
      rays({{1, -1}, {-1, -1}, {1, 0}, {-1, 0}, {1, 1}, {-1, 1}}), -1);
  def(FlyingOx, "+VM", "Flying Ox", "飛牛", "cy", {},
      rays({{1, -1}, {0, -1}, {-1, -1}, {1, 1}, {0, 1}, {-1, 1}}), -1);

  // Promotion back-links, special roles and restrictions.
  for (int k = 0; k < KindCount; ++k)
    if (t[k].promotesTo >= 0) t[t[k].promotesTo].base = k;
  t[King].royal = t[Prince].royal = true;
  t[Lion].lion = t[LionO].lion = true;
  // Pieces that would have no move on the last rank(s) must promote there.
  t[Pawn].deadRanks = 1;
  t[Lance].deadRanks = 1;
  t[Iron].deadRanks = 1;
  t[Stone].deadRanks = 1;
  t[Knight].deadRanks = 2;
  // No two unpromoted pawns / go-betweens of one side on a file (二歩・二仲人).
  t[Pawn].fileLimit = 1;
  t[GoBetween].fileLimit = 1;
  return t;
}

}  // namespace

const std::array<KindInfo, KindCount>& kindTable() {
  static const std::array<KindInfo, KindCount> table = buildTable();
  return table;
}

const std::vector<Dir>* continuation(Kind k, Dir first) {
  // Lion: any second king step after any first king step.
  static const std::vector<Dir> any = kKingSteps;
  // Horned falcon: forward lion power (second step forward again or back).
  static const std::vector<Dir> falcon = {{0, -1}, {0, 1}};
  // Soaring eagle: forward-diagonal lion power.
  static const std::vector<Dir> eagleL = {{1, -1}, {-1, 1}};
  static const std::vector<Dir> eagleR = {{-1, -1}, {1, 1}};
  const bool adjacent = first.dx >= -1 && first.dx <= 1 && first.dy >= -1 &&
                        first.dy <= 1 && (first.dx || first.dy);
  switch (k) {
    case Lion:
    case LionO:
      return adjacent ? &any : nullptr;
    case HornedFalcon:
      return (first.dx == 0 && first.dy == -1) ? &falcon : nullptr;
    case SoaringEagle:
      if (first.dy != -1) return nullptr;
      if (first.dx == 1) return &eagleL;
      if (first.dx == -1) return &eagleR;
      return nullptr;
    default:
      return nullptr;
  }
}

}  // namespace dai
