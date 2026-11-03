#include "library.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARDUINOJSON_ENABLE_ARDUINO_STRING 0
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#define ARDUINOJSON_USE_DOUBLE 1
#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
// Big JSON documents (a full recipe list) live in PSRAM.
struct PsramAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override {
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
};
static PsramAllocator g_alloc;
#define JSON_DOC JsonDocument doc(&g_alloc)
#else
#define JSON_DOC JsonDocument doc
#endif

using namespace ArduinoJson;

namespace {

// Numbers from the cloud sometimes arrive as strings; accept both.
double num(JsonVariantConst v, double dflt = 0) {
  if (v.isNull()) return dflt;
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    char *end;
    double d = strtod(s, &end);
    return end == s ? dflt : d;
  }
  if (v.is<bool>()) return v.as<bool>() ? 1 : 0;
  return v.as<double>();
}
long long inum(JsonVariantConst v, long long dflt = 0) {
  if (v.isNull()) return dflt;
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    char *end;
    long long d = strtoll(s, &end, 10);
    return end == s ? dflt : d;
  }
  if (v.is<long long>()) return v.as<long long>();
  return (long long)llround(num(v, (double)dflt));
}
void str(JsonVariantConst v, char *out, size_t n) {
  out[0] = 0;
  if (v.isNull()) return;
  if (v.is<const char *>()) {
    snprintf(out, n, "%s", v.as<const char *>());
  } else {
    double d = num(v);
    if (d == floor(d)) snprintf(out, n, "%lld", (long long)d);
    else snprintf(out, n, "%g", d);
  }
}
// Copy at most n-1 bytes without cutting a UTF-8 character in half.
void utf8_copy(char *dst, size_t n, const char *src) {
  size_t len = strlen(src);
  if (len >= n) {
    len = n - 1;
    while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) --len;
  }
  memcpy(dst, src, len);
  dst[len] = 0;
}
// " MAD ice coffee  " -> "MAD ice coffee" (the app strips names on save too)
void trim_copy(char *dst, size_t n, const char *src) {
  while (*src == ' ') ++src;
  utf8_copy(dst, n, src);
  for (size_t l = strlen(dst); l && dst[l - 1] == ' '; --l) dst[l - 1] = 0;
}

int find_code(JsonObjectConst o) {
  for (const char *k : {"code", "errorCode", "resultCode", "result", "status"}) {
    JsonVariantConst v = o[k];
    if (v.isNull()) continue;
    if (v.is<const char *>()) {
      const char *s = v.as<const char *>();
      char *end;
      long c = strtol(s, &end, 10);
      if (end != s && *end == 0) return (int)c;
      continue;
    }
    if (v.is<long long>() || v.is<double>()) return (int)num(v);
  }
  return -1;
}

const char *err_msg(JsonObjectConst o) {
  for (const char *k : {"msg", "message", "errorMsg", "errorMessage", "error", "reason", "tips", "info"}) {
    JsonVariantConst v = o[k];
    if (v.is<const char *>() && *v.as<const char *>()) return v.as<const char *>();
  }
  return "request failed";
}

bool parse_cloud_recipe(JsonObjectConst r, RecipeKind kind, PanelRecipe &p) {
  if (r["tableId"].isNull() || r["theName"].isNull()) return false;
  p = PanelRecipe();
  str(r["tableId"], p.id, sizeof p.id);
  trim_copy(p.r.name, sizeof p.r.name, r["theName"].as<const char *>() ? r["theName"].as<const char *>() : "");
  p.kind = kind;
  p.r.dose_g = num(r["dose"]);
  p.r.grind_size = (int)llround(num(r["grinderSize"]));
  p.r.grinder_on = inum(r["isSetGrinderSize"], 1) == 1;
  p.r.rpm = (int)inum(r["rpm"], 90);
  p.r.cup_type = (int)inum(r["cupType"], 3);
  p.bypass = inum(r["isEnableBypassWater"], 2) == 1;
  p.bypass_temp = num(r["bypassTemp"], 85);
  p.bypass_volume = num(r["bypassVolume"], 5);
  const char *col = r["theColor"] | "";
  p.color = *col ? library::parse_color(col) : 0xC9D5B8;
  utf8_copy(p.color_src, sizeof p.color_src, col);
  p.adapted_model = (int)inum(r["adaptedModel"], 1);
  p.created_ms = inum(r["createTimeStamp"], 0);
  p.is_shortcut = (int)inum(r["isShortcuts"], 0);
  p.subset_type = (int)inum(r["subSetType"], 0);
  p.subset_id = (int)inum(r["theSubsetId"], 0);
  JsonArrayConst pl = r["pourList"];
  for (JsonObjectConst q : pl) {
    if (p.r.n_pours >= xb::MAX_POURS) break;
    xb::Pour &o = p.r.pours[p.r.n_pours];
    o.volume_ml = num(q["volume"]);
    o.temp_c = num(q["temperature"], 92);
    o.pattern = (uint8_t)inum(q["pattern"], 2);
    o.flow = num(q["flowRate"], 3.0);
    o.pause_s = (uint8_t)inum(q["pausing"], 0);
    o.agitate_before = inum(q["isEnableVibrationBefore"], 2) == 1;
    o.agitate_after = inum(q["isEnableVibrationAfter"], 2) == 1;
    const char *nm = q["theName"] | "";
    utf8_copy(p.pour_names[p.r.n_pours], sizeof p.pour_names[0], nm);
    ++p.r.n_pours;
  }
  return true;
}

}  // namespace

namespace library {

uint32_t parse_color(const char *hex) {
  if (*hex == '#') ++hex;
  return (uint32_t)strtoul(hex, nullptr, 16) & 0xFFFFFF;
}

void color_hex(uint32_t c, char out[8]) { snprintf(out, 8, "#%06X", (unsigned)(c & 0xFFFFFF)); }

double ratio_denominator(const xb::Recipe &r) {
  if (r.dose_g <= 0) return 16.0;
  return round(r.total_water() / r.dose_g * 10) / 10;
}

const char *pour_name(const PanelRecipe &p, int i, char buf[24]) {
  if (p.pour_names[i][0]) return p.pour_names[i];
  if (i == 0) return "Bloom";
  snprintf(buf, 24, "Pour %d", i + 1);
  return buf;
}

bool parse_cloud_list(const char *json, size_t len, RecipeKind kind, std::vector<PanelRecipe> &out, std::string &err,
                      bool &token_expired) {
  token_expired = false;
  JSON_DOC;
  DeserializationError e = deserializeJson(doc, json, len);
  if (e) {
    err = std::string("bad reply from xBloom (") + e.c_str() + ")";
    return false;
  }
  JsonObjectConst o = doc.as<JsonObjectConst>();
  const char *res = o["result"] | "";
  if (strcmp(res, "success") != 0) {
    const int code = find_code(o);
    token_expired = code == 10001 || code == 20003;
    err = err_msg(o);
    return false;
  }
  for (JsonObjectConst r : o["list"].as<JsonArrayConst>()) {
    PanelRecipe p;
    if (parse_cloud_recipe(r, kind, p)) out.push_back(p);
  }
  return true;
}

std::string cloud_body(const PanelRecipe &p, const std::string &envelope_json, const char *table_id) {
  JSON_DOC;
  deserializeJson(doc, envelope_json);
  const xb::Recipe &r = p.r;
  char name[64];
  trim_copy(name, sizeof name, r.name);
  if (table_id) doc["tableId"] = table_id;
  doc["theName"] = name;
  doc["dose"] = r.dose_g;
  doc["grandWater"] = ratio_denominator(r);
  doc["grinderSize"] = (double)r.grind_size;
  doc["rpm"] = r.rpm;
  doc["cupType"] = r.cup_type;
  doc["adaptedModel"] = p.adapted_model;
  doc["isEnableBypassWater"] = p.bypass ? 1 : 2;
  doc["isSetGrinderSize"] = r.grinder_on ? 1 : 2;
  char col[8];
  color_hex(p.color, col);
  const bool same_colour = p.color_src[0] && parse_color(p.color_src) == p.color;
  doc["theColor"] = same_colour ? p.color_src : col;
  doc["theSubsetId"] = p.subset_id;
  doc["subSetType"] = p.subset_type ? p.subset_type : 2;
  doc["appPlace"].to<JsonArray>().add(4);
  doc["createTimeStamp"] = (long long)p.created_ms;
  doc["isShortcuts"] = p.is_shortcut ? p.is_shortcut : 2;
  if (p.bypass) {
    doc["bypassTemp"] = p.bypass_temp;
    doc["bypassVolume"] = p.bypass_volume;
  }
  // The pours travel as a JSON string inside the JSON body.
  JsonDocument pd;
  JsonArray arr = pd.to<JsonArray>();
  for (int i = 0; i < r.n_pours; ++i) {
    const xb::Pour &q = r.pours[i];
    char nb[24];
    JsonObject o = arr.add<JsonObject>();
    o["theName"] = pour_name(p, i, nb);
    o["volume"] = q.volume_ml;
    o["temperature"] = q.temp_c;
    o["flowRate"] = q.flow;
    o["pattern"] = q.pattern;
    o["pausing"] = q.pause_s;
    o["isEnableVibrationBefore"] = q.agitate_before ? 1 : 2;
    o["isEnableVibrationAfter"] = q.agitate_after ? 1 : 2;
  }
  std::string pours;
  serializeJson(pd, pours);
  doc["pourDataJSONStr"] = pours;
  std::string out;
  serializeJson(doc, out);
  return out;
}

// ---- local cache: our own compact format --------------------------------------
std::string to_cache_json(const std::vector<PanelRecipe> &lib) {
  JSON_DOC;
  doc["v"] = 1;
  JsonArray a = doc["recipes"].to<JsonArray>();
  for (const PanelRecipe &p : lib) {
    if (p.kind == KIND_STARTER) {  // built in; only the favourite flag needs keeping
      JsonObject o = a.add<JsonObject>();
      o["k"] = p.kind;
      o["n"] = p.r.name;
      o["fav"] = p.favorite;
      continue;
    }
    JsonObject o = a.add<JsonObject>();
    const xb::Recipe &r = p.r;
    o["k"] = p.kind;
    o["id"] = p.id;
    o["uid"] = p.uid;
    o["n"] = r.name;
    o["col"] = p.color;
    o["cs"] = p.color_src;
    o["fav"] = p.favorite;
    o["dirty"] = p.dirty;
    o["dose"] = r.dose_g;
    o["gs"] = r.grind_size;
    o["gon"] = r.grinder_on;
    o["rpm"] = r.rpm;
    o["cup"] = r.cup_type;
    o["ct"] = (long long)p.created_ms;
    o["sid"] = p.subset_id;
    o["st"] = p.subset_type;
    o["sc"] = p.is_shortcut;
    o["am"] = p.adapted_model;
    o["bp"] = p.bypass;
    o["bpt"] = p.bypass_temp;
    o["bpv"] = p.bypass_volume;
    JsonArray pa = o["p"].to<JsonArray>();
    for (int i = 0; i < r.n_pours; ++i) {
      const xb::Pour &q = r.pours[i];
      JsonArray e = pa.add<JsonArray>();
      e.add(q.volume_ml); e.add(q.temp_c); e.add(q.pattern); e.add(q.flow); e.add(q.pause_s);
      e.add(q.agitate_before); e.add(q.agitate_after); e.add(p.pour_names[i]);
    }
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool from_cache_json(const char *json, size_t len, std::vector<PanelRecipe> &out) {
  JSON_DOC;
  if (deserializeJson(doc, json, len)) return false;
  if ((doc["v"] | 0) != 1) return false;
  for (JsonObjectConst o : doc["recipes"].as<JsonArrayConst>()) {
    PanelRecipe p;
    p.kind = o["k"] | 0;
    utf8_copy(p.r.name, sizeof p.r.name, o["n"] | "");
    p.favorite = o["fav"] | false;
    if (p.kind == KIND_STARTER) {  // only the favourite flag; the recipe comes from firmware
      out.push_back(p);
      continue;
    }
    utf8_copy(p.id, sizeof p.id, o["id"] | "");
    p.uid = o["uid"] | 0u;
    p.color = o["col"] | 0xC9D5B8u;
    utf8_copy(p.color_src, sizeof p.color_src, o["cs"] | "");
    p.dirty = o["dirty"] | false;
    xb::Recipe &r = p.r;
    r.dose_g = o["dose"] | 15.0;
    r.grind_size = o["gs"] | 40;
    r.grinder_on = o["gon"] | true;
    r.rpm = o["rpm"] | 90;
    r.cup_type = o["cup"] | 3;
    p.created_ms = o["ct"] | 0ll;
    p.subset_id = o["sid"] | 0;
    p.subset_type = o["st"] | 2;
    p.is_shortcut = o["sc"] | 2;
    p.adapted_model = o["am"] | 1;
    p.bypass = o["bp"] | false;
    p.bypass_temp = o["bpt"] | 85.0;
    p.bypass_volume = o["bpv"] | 5.0;
    for (JsonArrayConst e : o["p"].as<JsonArrayConst>()) {
      if (r.n_pours >= xb::MAX_POURS) break;
      xb::Pour &q = r.pours[r.n_pours];
      q.volume_ml = e[0] | 0.0;
      q.temp_c = e[1] | 92.0;
      q.pattern = e[2] | 2;
      q.flow = e[3] | 3.0;
      q.pause_s = e[4] | 0;
      q.agitate_before = e[5] | false;
      q.agitate_after = e[6] | false;
      utf8_copy(p.pour_names[r.n_pours], sizeof p.pour_names[0], e[7] | "");
      ++r.n_pours;
    }
    out.push_back(p);
  }
  return true;
}

}  // namespace library
