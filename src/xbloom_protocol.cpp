// See xbloom_protocol.h. Ported from xbloom-py (MIT).
#include "xbloom_protocol.h"

#include <math.h>
#include <string.h>

#include <algorithm>

namespace xb {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static inline void put_u16(Bytes &b, uint16_t v) {
  b.push_back(v & 0xFF);
  b.push_back(v >> 8);
}
static inline void put_u32(Bytes &b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF);
}
static inline uint32_t rd_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint32_t float_bits(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}
// Python's round(): nearest, ties to even (default FP rounding mode).
static inline long py_round(double x) { return (long)nearbyint(x); }

double Recipe::total_water() const {
  double t = 0;
  for (int i = 0; i < n_pours; ++i) t += pours[i].volume_ml;
  return t;
}

const char *validate(const Recipe &r) {
  if (r.n_pours < 1 || r.n_pours > MAX_POURS) return "1-9 pours required";
  if (r.cup_type < 1 || r.cup_type > 4) return "bad cup type";
  static const double dose_min[5] = {0, 15, 5, 5, 1};
  static const double dose_max[5] = {0, 15, 18, 25, 5};
  if (r.dose_g < dose_min[r.cup_type] || r.dose_g > dose_max[r.cup_type]) return "dose out of range for cup";
  if (r.grinder_on) {
    if (r.grind_size < 1 || r.grind_size > 80) return "grind size must be 1-80";
    if (r.rpm < 60 || r.rpm > 120) return "grinder rpm must be 60-120";
  }
  for (int i = 0; i < r.n_pours; ++i) {
    const Pour &p = r.pours[i];
    if (p.volume_ml < 0 || p.volume_ml > 240) return "pour volume must be 0-240 ml";
    if (p.temp_c < 20 || p.temp_c > 98) return "temperature must be 20-98 C";
    if (p.flow < 3.0 - 1e-9 || p.flow > 3.5 + 1e-9) return "flow must be 3.0-3.5";
    if (p.pause_s > 59) return "pause must be 0-59 s";
    if (p.pattern < 1 || p.pattern > 3) return "bad pattern";
  }
  double w = r.total_water();
  if (w <= 0) return "no water";
  double ratio = w / r.dose_g;
  if (ratio < 5 || ratio > 25) return "ratio must be 1:5 - 1:25";
  return nullptr;
}

// ---------------------------------------------------------------------------
// frames
// ---------------------------------------------------------------------------
uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
  }
  return crc;
}

// 58 01 <type> [code LE16] [total_len LE32] 01 [data] [crc LE16]
Bytes build_frame(uint16_t code, const uint32_t *ints, size_t n_ints, const uint8_t *raw,
                  size_t raw_len, uint8_t frame_type) {
  Bytes body;
  if (raw) {
    body.assign(raw, raw + raw_len);
  } else {
    for (size_t i = 0; i < n_ints; ++i) put_u32(body, ints[i]);
  }
  Bytes f;
  f.reserve(12 + body.size());
  f.push_back(0x58);
  f.push_back(0x01);
  f.push_back(frame_type);
  put_u16(f, code);
  put_u32(f, (uint32_t)(12 + body.size()));
  f.push_back(0x01);
  f.insert(f.end(), body.begin(), body.end());
  put_u16(f, crc16(f.data(), f.size()));
  return f;
}

// The firmware only commits a recipe (incl. its grind size) when, in float32,
//   trunc((ratio_byte / 10) * dose) == sum(pour volumes on the wire).
// Pick such a ratio byte and spread that integer total across the pours.
static void gate_volumes(const std::vector<double> &src, int dose_i, std::vector<int> &out,
                         int &ratio_byte) {
  out.clear();
  double total = 0;
  for (double v : src) total += v;
  if (dose_i <= 0 || total <= 0 || src.empty()) {
    for (double v : src) out.push_back((int)py_round(v));
    ratio_byte = 0;
    return;
  }
  long r0 = py_round((total / dose_i) * 10);
  long chosen = -1, target = -1;
  const long cands[5] = {r0, r0 + 1, r0 - 1, r0 + 2, r0 - 2};
  for (long r : cands) {
    if (r <= 0 || r > 0xFF) continue;
    float prod = ((float)r / 10.0f) * (float)dose_i;
    double pd = prod;
    if ((long)pd == (long)(pd + 0.5)) {
      chosen = r;
      target = (long)pd;
      break;
    }
  }
  if (target < 0) {
    chosen = std::max(1L, std::min(255L, r0));
    target = (long)(double)(((float)chosen / 10.0f) * (float)dose_i);
  }
  std::vector<double> ideal(src.size());
  for (size_t i = 0; i < src.size(); ++i) ideal[i] = src[i] * (double)target / total;
  out.resize(src.size());
  long sum = 0;
  for (size_t i = 0; i < src.size(); ++i) {
    out[i] = (int)ideal[i];
    sum += out[i];
  }
  long deficit = target - sum;
  if (deficit > 0) {
    std::vector<size_t> order(src.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      return (ideal[a] - out[a]) > (ideal[b] - out[b]);
    });
    for (long k = 0; k < deficit && k < (long)order.size(); ++k) out[order[k]] += 1;
  }
  ratio_byte = (int)(chosen & 0xFF);
}

static uint8_t pattern_byte(uint8_t api) {
  switch (api) {
    case PAT_CENTERED: return 0;
    case PAT_SPIRAL: return 2;
    case PAT_CIRCULAR: return 1;
    default: return 0;
  }
}

static bool use_grinder(const Recipe &r) { return r.grinder_on && r.grind_size > 0; }

Bytes encode_recipe_blob(const Recipe &r) {
  const bool grind = use_grinder(r);
  const int dose_i = r.dose_g ? (int)py_round(r.dose_g) : 0;
  std::vector<double> src;
  for (int i = 0; i < r.n_pours; ++i) src.push_back(std::max(0.0, r.pours[i].volume_ml));
  std::vector<int> vols;
  int ratio_byte = 0;
  gate_volumes(src, dose_i, vols, ratio_byte);

  Bytes parts;
  for (int i = 0; i < r.n_pours; ++i) {
    const Pour &p = r.pours[i];
    int volume = vols[i];
    uint8_t temp = (uint8_t)(int)p.temp_c;
    uint8_t pat = pattern_byte(p.pattern);
    uint8_t vib = (p.agitate_before ? 1 : 0) | (p.agitate_after ? 2 : 0);
    uint8_t flow = (uint8_t)(py_round(p.flow * 10) & 0xFF);
    // volume in <=127 ml chunks
    if (volume > 127) {
      for (int k = 0; k < volume / 127; ++k) parts.insert(parts.end(), {127, temp, pat, vib});
      int rem = volume % 127;
      if (rem) parts.insert(parts.end(), {(uint8_t)rem, temp, pat, vib});
    } else {
      parts.insert(parts.end(), {(uint8_t)volume, temp, pat, vib});
    }
    uint8_t wait_neg = (uint8_t)((-(int)p.pause_s) & 0xFF);
    uint8_t rpm = (i == 0 && grind) ? (uint8_t)(r.rpm & 0xFF) : 0;
    parts.insert(parts.end(), {wait_neg, 0, rpm, flow});
  }
  Bytes blob;
  blob.push_back((uint8_t)(parts.size() & 0xFF));
  blob.insert(blob.end(), parts.begin(), parts.end());
  int gs = grind ? std::min(r.grind_size, 80) : 0;
  blob.push_back((uint8_t)gs);
  blob.push_back((uint8_t)ratio_byte);
  return blob;
}

std::vector<Bytes> build_load_frames(const Recipe &r) {
  std::vector<Bytes> out;
  const uint32_t hs[2] = {185, 1};
  out.push_back(build_frame(CMD_HANDSHAKE, hs, 2));

  const uint32_t dose[3] = {0, 0, (uint32_t)py_round(r.dose_g)};
  out.push_back(build_frame(CMD_BYPASS_DOSE, dose, 3));

  float cmax = 200.f, cmin = 80.f;  // xPod / Other / Tea
  if (r.cup_type == 2) { cmax = 110.f; cmin = 90.f; }  // Omni dripper
  const uint32_t cup[2] = {float_bits(cmax), float_bits(cmin)};
  out.push_back(build_frame(CMD_SET_CUP, cup, 2));

  Bytes blob = encode_recipe_blob(r);
  out.push_back(build_frame(use_grinder(r) ? CMD_RECIPE_GRIND : CMD_RECIPE_NO_GRIND, nullptr, 0,
                            blob.data(), blob.size()));
  return out;
}

Bytes frame_execute() { return build_frame(CMD_EXECUTE); }
Bytes frame_quit_recipe() { return build_frame(CMD_QUIT_RECIPE); }
Bytes frame_tare() { return build_frame(CMD_TARE); }
Bytes frame_pause() { return build_frame(CMD_BREW_PAUSE); }
Bytes frame_resume() { return build_frame(CMD_BREW_RESUME); }
Bytes frame_stop() { return build_frame(CMD_BREW_STOP); }

uint8_t pattern_to_byte(uint8_t api) { return pattern_byte(api); }
double temp_display_to_wire(int d) { return d <= 39 ? 20.0 : d >= 96 ? 98.0 : (double)d; }

static Bytes ints2(uint16_t code, uint32_t a, uint32_t b) {
  const uint32_t v[2] = {a, b};
  return build_frame(code, v, 2);
}
Bytes frame_grinder_enter(int size, int rpm) { return ints2(8006, size, rpm); }
Bytes frame_grinder_start(int size, int rpm) {
  const uint32_t v[3] = {1000, (uint32_t)size, (uint32_t)rpm};
  return build_frame(3500, v, 3);
}
Bytes frame_grinder_stop() { return build_frame(3505); }
Bytes frame_brewer_enter(uint8_t pb, double t) { return ints2(8007, pb, float_bits((float)(t * 10))); }
Bytes frame_brewer_start(double flow, double vol, double t, uint8_t pb) {
  const uint32_t v[5] = {float_bits((float)(flow * 10)), float_bits((float)(vol * 10)),
                         float_bits((float)(t * 10)), 0 /* tank */, pb};
  return build_frame(4506, v, 5);
}
Bytes frame_brewer_pause() { return build_frame(8019); }
Bytes frame_brewer_resume() { return build_frame(8021); }
Bytes frame_brewer_stop() { return build_frame(4507); }
Bytes frame_brewer_temp(double t) {
  const uint32_t v = (uint32_t)py_round(t * 10);
  return build_frame(4510, &v, 1);
}
Bytes frame_brewer_pattern(uint8_t pb) {
  const uint32_t v = pb;
  return build_frame(8016, &v, 1);
}
Bytes frame_scale_enter() { return build_frame(8003); }
Bytes frame_quit_grinder() { return build_frame(8012); }
Bytes frame_quit_brewer() { return build_frame(8013); }
Bytes frame_quit_scale() { return build_frame(8014); }
Bytes frame_weight_unit(WeightUnit u) {
  const uint32_t v = u;
  return build_frame(8005, &v, 1);
}

uint16_t frame_code(const Bytes &f) {
  if (f.size() < 5 || f[0] != 0x58) return 0;
  return f[3] | (f[4] << 8);
}

// ---------------------------------------------------------------------------
// notifications
// ---------------------------------------------------------------------------
uint32_t Event::u32() const { return len >= 4 ? rd_u32(payload) : 0; }
float Event::f32() const {
  if (len < 4) return 0;
  float f;
  memcpy(&f, payload, 4);
  return f;
}

bool parse_event(const Bytes &d, Event &e) {
  if (d.size() < 10 || d[0] != 0x58 || d[1] != 0x02) return false;
  e.code = d[3] | (d[4] << 8);
  if (d.size() > 12) {
    e.payload = d.data() + 10;
    e.len = d.size() - 12;
  } else {
    e.payload = nullptr;
    e.len = 0;
  }
  return true;
}

uint32_t refusal_code(const Bytes &f) {
  if (f.size() < 12) return 0;
  size_t plen = f.size() - 12;
  if (plen < 16) return 0;
  const uint8_t *p = f.data() + 10;
  return ((uint32_t)p[12] << 16) | ((uint32_t)p[13] << 8) | p[14];  // big-endian 3 bytes
}

const char *refusal_reason(uint32_t code) {
  if (!code) return nullptr;
  if (code & 0x000040) return "No water";
  if (code & 0x001800) return "Go to the home screen and retry";
  if (code & 0x100000) return "Machine not ready";
  if (code & 0x200000) return "Power-cycle restore incomplete";
  if (code & 0x400000) return "Recipe rejected";
  if (code & (0x000100 | 0x000200 | 0x000400 | 0x004000 | 0x010000 | 0x020000 | 0x040000 |
              0x080000 | 0x800000))
    return "Machine busy";
  return nullptr;
}

const char *activity_name(uint32_t a) {
  switch (a) {
    case 1: return "Home";
    case 3: return "Brewer";
    case 65: return "Auto";
    case 4:
    case 5: return "Scale";
    case 29: return "Starting";
    case 34: return "Grinding";
    case 30:
    case 35:
    case 16: return "Brewing";
    case 31: return "Paused";
    case 36: return "Done";
    default: return nullptr;
  }
}

const char *fault_name(uint16_t c) {
  switch (c) {
    case N_FAULT_NO_WATER: return "Out of water";
    case N_FAULT_NO_BEANS: return "No beans";
    case N_FAULT_DOSE_WATER: return "Dose/water error";
    case N_FAULT_GEAR: return "Grinder gear error";
    default: return nullptr;
  }
}

static const size_t MAX_FRAME = 1024;

// Declared total length of a 5802 frame at the start of d, or 0.
static size_t frame_len(const uint8_t *d, size_t n) {
  if (n < 9 || d[0] != 0x58 || d[1] != 0x02) return 0;
  uint32_t L = rd_u32(d + 5);
  return (L >= 12 && L <= MAX_FRAME) ? L : 0;
}

static bool crc_ok(const uint8_t *f, size_t n) {
  if (n < 12) return false;
  uint16_t want = f[n - 2] | (f[n - 1] << 8);
  return crc16(f, n - 2) == want;
}

static bool could_open(const uint8_t *d, size_t n) {
  // starts like "58 02" and is too short to read its length yet
  static const uint8_t hdr[2] = {0x58, 0x02};
  size_t k = std::min<size_t>(n, 2);
  if (memcmp(d, hdr, k) == 0 && n < 9) return true;
  size_t L = frame_len(d, n);
  return L && L > n;
}

// Whole frames at the start of d; returns bytes consumed.
static size_t walk(const uint8_t *d, size_t n, const std::function<void(const Bytes &)> &cb,
                   int &count) {
  size_t i = 0;
  count = 0;
  while (i < n) {
    size_t L = frame_len(d + i, n - i);
    if (!L || L > n - i) break;
    cb(Bytes(d + i, d + i + L));
    ++count;
    i += L;
  }
  return i;
}

void Assembler::feed(const uint8_t *data, size_t len,
                     const std::function<void(const Bytes &)> &on_frame) {
  Bytes buf;
  bool emitted = false;
  if (!pending_.empty()) {
    Bytes joined = pending_;
    joined.insert(joined.end(), data, data + len);
    size_t L = frame_len(joined.data(), joined.size());
    if (L && L > joined.size()) {
      pending_ = joined;  // still incomplete
      return;
    }
    pending_.clear();
    if (L && crc_ok(joined.data(), L)) {
      on_frame(Bytes(joined.begin(), joined.begin() + L));
      emitted = true;
      buf.assign(joined.begin() + L, joined.end());
    } else {
      buf.assign(data, data + len);  // pending was junk; read this one alone
    }
  } else {
    buf.assign(data, data + len);
  }
  if (buf.empty()) return;
  int count = 0;
  size_t used = walk(buf.data(), buf.size(), on_frame, count);
  if (count == 0 && !emitted && !could_open(buf.data(), buf.size())) {
    on_frame(buf);  // not walkable: decode whole, as a lone frame
    return;
  }
  if (used < buf.size()) {
    const uint8_t *rest = buf.data() + used;
    size_t rn = buf.size() - used;
    if (could_open(rest, rn)) pending_.assign(rest, rest + rn);
  }
}

}  // namespace xb
