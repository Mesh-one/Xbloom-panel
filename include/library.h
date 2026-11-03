// The recipe library: what the panel shows and brews.
//
// Sources: your xBloom account (Created + Shared), recipes made on the panel,
// and a few built-in starters. Stored on the panel (/recipes.json) so it works
// offline and survives restarts.
//
// Also converts to and from the xBloom cloud's JSON (field mapping follows
// xbloom-py's client.py / cloud.py). No Arduino code: unit-tested on a PC.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

#include "xbloom_protocol.h"

enum RecipeKind : uint8_t {
  KIND_MINE = 0,     // "Created": in your account, or made on the panel
  KIND_SHARED = 1,   // "Shared": saved into your account from a share link
  KIND_STARTER = 2,  // built into the panel
  KIND_SEED = 3,     // your recipes typed into the firmware before sign-in; replaced by the account copy
};

struct PanelRecipe {
  xb::Recipe r;
  char id[24] = "";          // xBloom cloud tableId; "" = not in the account (yet)
  uint32_t uid = 0;          // stable local id (screens refer to recipes by this)
  uint32_t color = 0xC9D5B8; // tile colour
  char color_src[10] = "";    // the account's own spelling of it ("#b0bde0"), sent back unchanged
  uint8_t kind = KIND_MINE;
  bool favorite = false;
  bool dirty = false;        // edited on the panel, not yet saved to the account
  // Account fields kept exactly as received, so saving from the panel never
  // resets something set in the app.
  int64_t created_ms = 0;
  int subset_id = 0, subset_type = 2, is_shortcut = 2, adapted_model = 1;
  bool bypass = false;
  double bypass_temp = 85, bypass_volume = 5;
  char pour_names[xb::MAX_POURS][24] = {};
};

namespace library {

// ---- local file --------------------------------------------------------------
std::string to_cache_json(const std::vector<PanelRecipe> &lib);
bool from_cache_json(const char *json, size_t len, std::vector<PanelRecipe> &out);

// ---- xBloom cloud ---------------------------------------------------------------
// Parse a whole list response ({"result":"success","list":[...]}); kind = MINE or SHARED.
// Returns false if the response isn't a success; `err` gets the server's message and
// `token_expired` is set for the "sign in again" codes (10001 / 20003).
bool parse_cloud_list(const char *json, size_t len, RecipeKind kind, std::vector<PanelRecipe> &out,
                      std::string &err, bool &token_expired);
// The create/update body fields (without the auth envelope).
std::string cloud_body(const PanelRecipe &p, const std::string &envelope_json, const char *table_id_or_null);

// ---- helpers --------------------------------------------------------------------
uint32_t parse_color(const char *hex);           // "#C9D5B8" -> 0xC9D5B8
void color_hex(uint32_t c, char out[8]);          // 0xC9D5B8 -> "#C9D5B8"
double ratio_denominator(const xb::Recipe &r);    // total water / dose, 1 decimal
const char *pour_name(const PanelRecipe &p, int i, char buf[24]);  // stored name or Bloom / Pour n

}  // namespace library
