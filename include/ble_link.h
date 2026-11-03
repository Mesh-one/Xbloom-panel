// Bluetooth link to the xBloom Studio. Runs in its own FreeRTOS task.
// The UI talks to it only through two queues, so LVGL is never touched from here.
#pragma once

#include <stdint.h>
#include "xbloom_protocol.h"

enum class LinkState : uint8_t { Idle, Scanning, Connecting, Connected };

enum class UiEvType : uint8_t {
  Link,        // i = LinkState, msg = machine name
  Weight,      // f = grams
  Water,       // f = ml poured
  Activity,    // i = raw activity code
  PourIndex,   // i = 0-based pour index
  BrewDone,
  BrewAbandoned,
  BrewPaused,
  BrewResumed,
  Fault,       // msg
  CmdResult,   // i = BleCmdType, ok, msg = reason when !ok
  Sleep,       // i = 1 sleeping, 0 awake
  PodScanned,  // msg = pod id
};

struct UiEvent {
  UiEvType type;
  int32_t i = 0;
  float f = 0;
  bool ok = true;
  char msg[48] = {0};
};

enum class BleCmdType : uint8_t {
  // recipe brew
  Load, Execute, Tare, Pause, Resume, Stop, QuitRecipe,
  // manual modules (a/b/x/y/z per comment)
  GrinderEnter,   // a = size, b = rpm
  GrinderStart,   // a = size, b = rpm
  GrinderStop,
  BrewerEnter,    // a = pattern byte, x = wire temp
  BrewerStart,    // x = flow, y = volume, z = wire temp, a = pattern byte
  BrewerStop,
  BrewerTemp,     // x = wire temp
  BrewerPattern,  // a = pattern byte
  ScaleEnter,
  QuitGrinder, QuitBrewer, QuitScale,
  WeightUnit,     // a = xb::WeightUnit
};

struct BleCmd {
  BleCmdType type;
  int32_t a = 0, b = 0;
  double x = 0, y = 0, z = 0;
  xb::Recipe recipe;  // Load only
};

namespace ble_link {
void begin();                    // start the BLE task (scans + auto-reconnects)
bool post(const BleCmd &cmd);    // from UI; false if the queue is full
bool poll(UiEvent &ev);          // from UI loop; non-blocking
LinkState state();
}  // namespace ble_link
