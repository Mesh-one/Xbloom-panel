// Native test driver. Mode "enc": reads recipes from stdin, prints load-frame hex.
// Mode "asm": reads hex notifications (one per line, blank line = new session),
// prints hex of each frame the Assembler emits.
#include <stdio.h>
#include <string.h>
#include <string>
#include <iostream>
#include <sstream>
#include "xbloom_protocol.h"

static std::string hex(const xb::Bytes &b) {
  static const char *h = "0123456789abcdef";
  std::string s;
  for (uint8_t c : b) { s += h[c >> 4]; s += h[c & 15]; }
  return s;
}

int main(int argc, char **argv) {
  std::string mode = argc > 1 ? argv[1] : "enc";
  std::string line;
  if (mode == "enc") {
    while (std::getline(std::cin, line)) {
      if (line.empty()) continue;
      std::istringstream in(line);
      xb::Recipe r;
      int gon, n;
      in >> r.dose_g >> r.grind_size >> r.rpm >> r.cup_type >> gon >> n;
      r.grinder_on = gon;
      r.n_pours = n;
      for (int i = 0; i < n; ++i) {
        int pat, pause, ab, aa;
        xb::Pour &p = r.pours[i];
        in >> p.volume_ml >> p.temp_c >> pat >> p.flow >> pause >> ab >> aa;
        p.pattern = pat; p.pause_s = pause; p.agitate_before = ab; p.agitate_after = aa;
      }
      auto fr = xb::build_load_frames(r);
      for (auto &f : fr) printf("%s ", hex(f).c_str());
      printf("%s\n", hex(xb::frame_execute()).c_str());
    }
  } else if (mode == "mod") {
    using namespace xb;
    printf("%s\n%s\n%s\n", hex(frame_grinder_enter(36, 90)).c_str(), hex(frame_grinder_start(36, 90)).c_str(), hex(frame_grinder_stop()).c_str());
    printf("%s\n%s\n", hex(frame_brewer_enter(1, 85)).c_str(), hex(frame_brewer_start(3.5, 250, 85, 1)).c_str());
    printf("%s\n%s\n", hex(frame_brewer_temp(92.5)).c_str(), hex(frame_brewer_pattern(2)).c_str());
    printf("%s\n%s\n%s\n%s\n", hex(frame_scale_enter()).c_str(), hex(frame_quit_grinder()).c_str(), hex(frame_quit_brewer()).c_str(), hex(frame_quit_scale()).c_str());
    printf("%s\n%s\n%s\n", hex(frame_weight_unit(UNIT_G)).c_str(), hex(frame_weight_unit(UNIT_OZ)).c_str(), hex(frame_weight_unit(UNIT_ML)).c_str());
  } else {
    xb::Assembler a;
    while (std::getline(std::cin, line)) {
      if (line.empty()) { a.reset(); printf("--\n"); continue; }
      xb::Bytes d;
      for (size_t i = 0; i + 1 < line.size(); i += 2) d.push_back(std::stoi(line.substr(i, 2), nullptr, 16));
      a.feed(d.data(), d.size(), [](const xb::Bytes &f) { printf("%s\n", hex(f).c_str()); });
    }
  }
  return 0;
}
