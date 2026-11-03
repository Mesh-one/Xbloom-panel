// xBloom Studio BLE protocol — frame building, recipe encoding, notification parsing.
//
// Ported from xbloom-py (MIT, (c) Mansour Alshekhi) — https://github.com/Alshekhi/xbloom-py
// Unofficial: not affiliated with or endorsed by xBloom. The protocol was worked
// out by inspection and a machine firmware update can change it at any time.
//
// Pure C++ with no Arduino dependency, so it can be unit-tested on a PC.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <functional>
#include <vector>

namespace xb {

using Bytes = std::vector<uint8_t>;

// ---- GATT -----------------------------------------------------------------
// Write commands to FFE1, receive echoes + live events on FFE2 (notify).
static const char *const UUID_TX = "0000ffe1-0000-1000-8000-00805f9b34fb";
static const char *const UUID_RX = "0000ffe2-0000-1000-8000-00805f9b34fb";
static const char *const NAME_PREFIX = "XBLOOM";  // advertised name, e.g. "XBLOOM 1234"

// ---- Command codes (host -> machine) --------------------------------------
enum Cmd : uint16_t {
  CMD_HANDSHAKE = 8100,
  CMD_BYPASS_DOSE = 8102,
  CMD_SET_CUP = 8104,
  CMD_RECIPE_GRIND = 8001,
  CMD_RECIPE_NO_GRIND = 8004,
  CMD_EXECUTE = 8002,      // starts the brew — only ever sent on an explicit user tap
  CMD_QUIT_RECIPE = 8017,  // drop a loaded-but-not-started recipe, go home
  CMD_TARE = 8500,
  CMD_BREW_PAUSE = 40518,
  CMD_BREW_RESUME = 40524,
  CMD_BREW_STOP = 40519,
};

// ---- Notification codes (machine -> host) ---------------------------------
enum Notify : uint16_t {
  N_ACTIVITY = 8023,     // uint32 state, see activity_name()
  N_WEIGHT = 20501,      // float grams
  N_WEIGHT_ALT = 10507,  // float grams
  N_WATER = 40523,       // float ml poured
  N_BLOOM = 40510,       // uint32 pour index
  N_ENJOY = 40512,       // brew complete
  N_ABANDONED = 40513,   // brew left from pause
  N_MACHINE_INFO = 40521,
  N_SLEEPING = 8009,
  N_AWAKE = 8011,
  N_PODS = 40501,        // recipe card scanned, ASCII pod id
  N_FAULT_NO_WATER = 40522,
  N_FAULT_NO_BEANS = 40517,
  N_FAULT_DOSE_WATER = 8204,
  N_FAULT_GEAR = 8203,
};

// ---- Recipes ----------------------------------------------------------------
// Pour pattern, as the xBloom app/API numbers it.
enum Pattern : uint8_t { PAT_CENTERED = 1, PAT_SPIRAL = 2, PAT_CIRCULAR = 3 };

// Cup types: 1 = xPod, 2 = Omni dripper, 3 = Other, 4 = Tea.
static const int MAX_POURS = 9;

struct Pour {
  double volume_ml = 60;   // 0..240
  double temp_c = 92;      // 20 (room temp) .. 98 (boiling)
  uint8_t pattern = PAT_SPIRAL;
  double flow = 3.0;       // 3.0..3.5 ml/s
  uint8_t pause_s = 0;     // wait after this pour, 0..59
  bool agitate_before = false;
  bool agitate_after = false;
};

struct Recipe {
  char name[64] = "Recipe";
  double dose_g = 15;      // grams
  int grind_size = 40;     // 1..80; 0 = grinder off
  int rpm = 90;            // 60..120
  int cup_type = 3;
  bool grinder_on = true;
  uint8_t n_pours = 0;
  Pour pours[MAX_POURS];

  double total_water() const;
};

// Returns nullptr when the recipe is within the machine's ranges, else a
// human-readable reason. Always validate before sending.
const char *validate(const Recipe &r);

// ---- Frames ------------------------------------------------------------------
uint16_t crc16(const uint8_t *data, size_t len);  // CRC16-Kermit
Bytes build_frame(uint16_t code, const uint32_t *ints = nullptr, size_t n_ints = 0,
                  const uint8_t *raw = nullptr, size_t raw_len = 0, uint8_t frame_type = 1);
Bytes encode_recipe_blob(const Recipe &r);

// The frames that LOAD a recipe onto the machine, in send order:
// handshake, dose, cup range, recipe. Nothing here starts a brew.
std::vector<Bytes> build_load_frames(const Recipe &r);

Bytes frame_execute();      // start the loaded brew
Bytes frame_quit_recipe();
Bytes frame_tare();
Bytes frame_pause();
Bytes frame_resume();
Bytes frame_stop();

// ---- Manual modules (the machine's Grinder / Brewer / Scale screens) -------
// Pattern bytes on the wire: 0 = centered, 1 = circular, 2 = spiral.
uint8_t pattern_to_byte(uint8_t api_pattern);
double temp_display_to_wire(int display_c);  // 39 -> 20 (room temp), 96 -> 98 (boil)

Bytes frame_grinder_enter(int size, int rpm);  // go to grinder screen, set size/rpm (no motor)
Bytes frame_grinder_start(int size, int rpm);
Bytes frame_grinder_stop();
Bytes frame_brewer_enter(uint8_t pattern_byte, double temp_c);
Bytes frame_brewer_start(double flow, double volume_ml, double temp_c, uint8_t pattern_byte);  // pours hot water
Bytes frame_brewer_pause();    // 8019 (named in the official app; not yet live-tested by xbloom-py)
Bytes frame_brewer_resume();   // 8021 (same)
Bytes frame_brewer_stop();     // 4507 (same)
Bytes frame_brewer_temp(double temp_c);
Bytes frame_brewer_pattern(uint8_t pattern_byte);
Bytes frame_scale_enter();
Bytes frame_quit_grinder();
Bytes frame_quit_brewer();
Bytes frame_quit_scale();
enum WeightUnit : uint8_t { UNIT_G = 1, UNIT_ML = 0, UNIT_OZ = 2 };  // xbloom-py spec codes
Bytes frame_weight_unit(WeightUnit u);

// Command code carried by a frame (ours or the machine's), or 0.
uint16_t frame_code(const Bytes &f);

// ---- Notifications ----------------------------------------------------------
struct Event {
  uint16_t code = 0;
  const uint8_t *payload = nullptr;
  size_t len = 0;
  bool has_u32() const { return len >= 4; }
  uint32_t u32() const;
  float f32() const;
};

// Error field in a command reply; 0 = accepted.
uint32_t refusal_code(const Bytes &frame);
const char *refusal_reason(uint32_t code);  // nullptr if no known bit
const char *activity_name(uint32_t activity);
const char *fault_name(uint16_t code);      // nullptr if not a fault

// Splits FFE2 notifications into frames. The machine packs several frames into
// one notification and may split one frame across two. Feed every notification,
// in order; get back whole, checksum-valid frames.
class Assembler {
 public:
  void reset() { pending_.clear(); }
  void feed(const uint8_t *data, size_t len, const std::function<void(const Bytes &)> &on_frame);

 private:
  Bytes pending_;
};

bool parse_event(const Bytes &frame, Event &out);

}  // namespace xb
