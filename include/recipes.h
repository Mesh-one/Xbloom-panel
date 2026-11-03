// Recipes built into the firmware:
//  - starters: always available under Recipes > Starter
//  - seeds: your two recipes, typed in from the app screenshots, so the panel is
//    useful before you sign in. Signing in replaces them with the account copies.
#pragma once
#include <string.h>

#include <vector>

#include "library.h"

namespace recipes {

enum { CUP_XPOD = 1, CUP_OMNI = 2, CUP_OTHER = 3, CUP_TEA = 4 };

inline const char *cup_name(int cup) {
  switch (cup) {
    case CUP_XPOD: return "xPod";
    case CUP_OMNI: return "Omni";
    case CUP_TEA: return "Tea";
    default: return "Other";
  }
}

// Dose range the machine accepts for each cup type (from xbloom-py's spec).
inline void dose_range(int cup, double &lo, double &hi) {
  switch (cup) {
    case CUP_XPOD: lo = hi = 15; break;
    case CUP_OMNI: lo = 5; hi = 18; break;
    case CUP_TEA: lo = 1; hi = 5; break;
    default: lo = 5; hi = 25; break;
  }
}

// ml, °C, pattern, wait after (s), agitate after, flow
inline xb::Pour P(double ml, double temp, uint8_t pattern, uint8_t pause_s, bool agitate = false,
                  double flow = 3.0) {
  xb::Pour p;
  p.volume_ml = ml;
  p.temp_c = temp;
  p.pattern = pattern;
  p.pause_s = pause_s;
  p.agitate_after = agitate;
  p.flow = flow;
  return p;
}

inline PanelRecipe R(const char *name, int cup, double dose, int grind, int rpm, uint32_t color, RecipeKind kind,
                     bool fav, std::initializer_list<xb::Pour> pours) {
  PanelRecipe pr;
  strncpy(pr.r.name, name, sizeof(pr.r.name) - 1);
  pr.r.cup_type = cup;
  pr.r.dose_g = dose;
  pr.r.grind_size = grind;
  pr.r.rpm = rpm;
  pr.r.grinder_on = true;
  for (const xb::Pour &p : pours) pr.r.pours[pr.r.n_pours++] = p;
  pr.color = color;
  pr.kind = kind;
  pr.favorite = fav;
  return pr;
}

inline std::vector<PanelRecipe> seeds() {
#ifdef PUBLIC_BUILD
  return {};  // public build: everyone starts with their own recipes from their account
#endif
  using namespace xb;
  return {
      R("MAD ice coffee", CUP_OTHER, 22, 36, 90, 0xC8D6B5, KIND_SEED, true,
        {P(60, 95, PAT_CIRCULAR, 25), P(80, 85, PAT_SPIRAL, 30, true), P(80, 85, PAT_CENTERED, 5, true)}),
      R("ICE coffee", CUP_OMNI, 18, 36, 90, 0xB0BDE0, KIND_SEED, true,
        {P(50, 95, PAT_SPIRAL, 40), P(70, 95, PAT_SPIRAL, 5, true), P(60, 94, PAT_CENTERED, 5)}),
  };
}

// Starting points only: dial in the grind for your beans.
inline std::vector<PanelRecipe> starters() {
  using namespace xb;
  return {
      R("Daily Pour Over", CUP_OTHER, 15, 45, 90, 0xE2DBA6, KIND_STARTER, false,
        {P(45, 93, PAT_CENTERED, 30), P(95, 93, PAT_SPIRAL, 10), P(100, 93, PAT_SPIRAL, 0)}),
      R("Light Roast", CUP_OTHER, 15, 42, 100, 0xE4A3A4, KIND_STARTER, false,
        {P(50, 96, PAT_CENTERED, 35), P(105, 96, PAT_SPIRAL, 10), P(100, 96, PAT_SPIRAL, 0, false, 3.5)}),
      R("Dark Roast", CUP_OTHER, 16, 52, 80, 0xE6C3A0, KIND_STARTER, false,
        {P(40, 88, PAT_CENTERED, 30), P(100, 88, PAT_CIRCULAR, 10), P(100, 88, PAT_CIRCULAR, 0)}),
  };
}

}  // namespace recipes
