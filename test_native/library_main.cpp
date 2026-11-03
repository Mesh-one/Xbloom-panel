// Native test driver for library.cpp. Reads a cloud list response on stdin.
//   parse: prints one JSON line per recipe: {"parsed":{...}, "body":<cloud body>}
//   cache: cloud list -> cache JSON -> back; prints "same N" or "DIFF i"
#include <stdio.h>
#include <string.h>
#include <iostream>
#include <iterator>
#include <string>
#include "library.h"

static void dump(const PanelRecipe &p) {
  const xb::Recipe &r = p.r;
  printf("{\"id\":\"%s\",\"name\":\"%s\",\"dose\":%.6g,\"gs\":%d,\"gon\":%d,\"rpm\":%d,\"cup\":%d,\"bp\":%d,"
         "\"bpt\":%.6g,\"bpv\":%.6g,\"col\":%u,\"am\":%d,\"ct\":%lld,\"sc\":%d,\"st\":%d,\"sid\":%d,\"pours\":[",
         p.id, r.name, r.dose_g, r.grind_size, r.grinder_on, r.rpm, r.cup_type, p.bypass, p.bypass_temp,
         p.bypass_volume, p.color, p.adapted_model, (long long)p.created_ms, p.is_shortcut, p.subset_type,
         p.subset_id);
  for (int i = 0; i < r.n_pours; ++i) {
    const xb::Pour &q = r.pours[i];
    printf("%s[%.6g,%.6g,%d,%.6g,%d,%d,%d,\"%s\"]", i ? "," : "", q.volume_ml, q.temp_c, q.pattern, q.flow,
           q.pause_s, q.agitate_before, q.agitate_after, p.pour_names[i]);
  }
  printf("]}");
}

static bool same(const PanelRecipe &a, const PanelRecipe &b) {
  if (strcmp(a.id, b.id) || strcmp(a.r.name, b.r.name) || a.r.dose_g != b.r.dose_g || a.r.grind_size != b.r.grind_size ||
      a.r.grinder_on != b.r.grinder_on || a.r.rpm != b.r.rpm || a.r.cup_type != b.r.cup_type || a.color != b.color || strcmp(a.color_src, b.color_src) ||
      a.kind != b.kind || a.created_ms != b.created_ms || a.subset_id != b.subset_id || a.subset_type != b.subset_type ||
      a.is_shortcut != b.is_shortcut || a.adapted_model != b.adapted_model || a.bypass != b.bypass ||
      a.bypass_temp != b.bypass_temp || a.bypass_volume != b.bypass_volume || a.r.n_pours != b.r.n_pours)
    return false;
  for (int i = 0; i < a.r.n_pours; ++i) {
    const xb::Pour &x = a.r.pours[i], &y = b.r.pours[i];
    if (x.volume_ml != y.volume_ml || x.temp_c != y.temp_c || x.pattern != y.pattern || x.flow != y.flow ||
        x.pause_s != y.pause_s || x.agitate_before != y.agitate_before || x.agitate_after != y.agitate_after ||
        strcmp(a.pour_names[i], b.pour_names[i]))
      return false;
  }
  return true;
}

int main(int argc, char **argv) {
  std::string mode = argc > 1 ? argv[1] : "parse";
  std::string in((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  std::vector<PanelRecipe> lib;
  std::string err;
  bool expired;
  if (!library::parse_cloud_list(in.data(), in.size(), KIND_MINE, lib, err, expired)) {
    printf("ERR %s expired=%d\n", err.c_str(), expired);
    return 0;
  }
  if (mode == "parse") {
    const std::string env = "{\"interfaceVersion\":20240918,\"memberId\":7,\"token\":\"t\"}";
    for (auto &p : lib) {
      printf("{\"parsed\":");
      dump(p);
      printf(",\"body\":%s}\n", library::cloud_body(p, env, p.id).c_str());
    }
  } else {
    for (size_t i = 0; i < lib.size(); ++i) { lib[i].uid = 1000 + i; lib[i].favorite = i % 2; lib[i].dirty = i % 3 == 0; }
    std::string c = library::to_cache_json(lib);
    std::vector<PanelRecipe> back;
    if (!library::from_cache_json(c.data(), c.size(), back) || back.size() != lib.size()) { printf("LOADFAIL\n"); return 1; }
    for (size_t i = 0; i < lib.size(); ++i)
      if (!same(lib[i], back[i]) || back[i].uid != lib[i].uid || back[i].favorite != lib[i].favorite ||
          back[i].dirty != lib[i].dirty) { printf("DIFF %zu\n", i); return 1; }
    printf("same %zu\n", lib.size());
  }
  return 0;
}
