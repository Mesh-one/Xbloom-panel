#include "ble_link.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

using namespace xb;

namespace {

struct RawNotify {
  uint16_t len;
  uint8_t data[244];
};

QueueHandle_t q_raw;   // NimBLE host task -> BLE task
QueueHandle_t q_cmd;   // UI -> BLE task
QueueHandle_t q_ui;    // BLE task -> UI

volatile LinkState g_state = LinkState::Idle;
NimBLEClient *g_client = nullptr;
NimBLERemoteCharacteristic *g_tx = nullptr;
Assembler g_asm;
bool g_sleeping = false;

// echo tracking for the command being sent
uint16_t g_wait_code = 0;
bool g_got_echo = false;
uint32_t g_echo_refusal = 0;

// Timing from xbloom-py: a brew step waits 3 s for its echo, re-sent once.
const uint32_t STEP_TIMEOUT_MS = 3000;
const int STEP_ATTEMPTS = 2;

void ui_send(const UiEvent &e) { xQueueSend(q_ui, &e, 0); }

void ui_msg(UiEvType t, const char *msg, int32_t i = 0, bool ok = true) {
  UiEvent e;
  e.type = t;
  e.i = i;
  e.ok = ok;
  if (msg) strlcpy(e.msg, msg, sizeof(e.msg));
  ui_send(e);
}

void set_state(LinkState s, const char *name = nullptr) {
  g_state = s;
  ui_msg(UiEvType::Link, name, (int32_t)s);
}

void on_notify(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  RawNotify r;
  r.len = (uint16_t)min(len, sizeof(r.data));
  memcpy(r.data, data, r.len);
  xQueueSend(q_raw, &r, 0);  // never block the BLE host task
}

void handle_frame(const Bytes &frame) {
  // command echo?
  uint16_t code = frame_code(frame);
  if (g_wait_code && code == g_wait_code && !g_got_echo) {
    g_got_echo = true;
    g_echo_refusal = refusal_code(frame);
  }
  Event ev;
  if (!parse_event(frame, ev)) return;
  UiEvent u;
  switch (ev.code) {
    case N_WEIGHT:
    case N_WEIGHT_ALT:
      if (!ev.has_u32()) return;
      u.type = UiEvType::Weight; u.f = ev.f32(); ui_send(u); break;
    case N_WATER:
      if (!ev.has_u32()) return;
      u.type = UiEvType::Water; u.f = ev.f32(); ui_send(u); break;
    case N_ACTIVITY:
      if (!ev.has_u32()) return;
      u.type = UiEvType::Activity; u.i = (int32_t)ev.u32(); ui_send(u); break;
    case N_BLOOM:
      if (!ev.has_u32()) return;
      u.type = UiEvType::PourIndex; u.i = (int32_t)ev.u32(); ui_send(u); break;
    case N_ENJOY: u.type = UiEvType::BrewDone; ui_send(u); break;
    case N_ABANDONED: u.type = UiEvType::BrewAbandoned; ui_send(u); break;
    case 40515: case 9009: case 9010: u.type = UiEvType::BrewPaused; ui_send(u); break;
    case 40516: case 9011: u.type = UiEvType::BrewResumed; ui_send(u); break;
    case N_SLEEPING: g_sleeping = true; u.type = UiEvType::Sleep; u.i = 1; ui_send(u); break;
    case N_AWAKE: g_sleeping = false; u.type = UiEvType::Sleep; u.i = 0; ui_send(u); break;
    case N_PODS: {
      u.type = UiEvType::PodScanned;
      size_t n = min(ev.len, sizeof(u.msg) - 1);
      for (size_t k = 0; k < n; ++k) u.msg[k] = isprint(ev.payload[k]) ? ev.payload[k] : 0;
      ui_send(u);
      break;
    }
    default:
      if (const char *f = fault_name(ev.code)) ui_msg(UiEvType::Fault, f);
  }
}

// Drain notifications for up to `ms`; returns early once the awaited echo arrives.
void pump(uint32_t ms) {
  uint32_t start = millis();
  RawNotify r;
  do {
    uint32_t left = ms - min(ms, (uint32_t)(millis() - start));
    if (xQueueReceive(q_raw, &r, pdMS_TO_TICKS(min<uint32_t>(left, 20)))) {
      g_asm.feed(r.data, r.len, handle_frame);
      while (xQueueReceive(q_raw, &r, 0)) g_asm.feed(r.data, r.len, handle_frame);
    }
    if (g_wait_code && g_got_echo) return;
  } while (millis() - start < ms);
}

// Write one frame and wait for the machine to echo its code.
// Returns nullptr on success, else a reason.
const char *send_confirmed(const Bytes &frame) {
  if (!g_client || !g_client->isConnected() || !g_tx) return "Not connected";
  const uint16_t code = frame_code(frame);
  for (int attempt = 0; attempt < STEP_ATTEMPTS; ++attempt) {
    g_wait_code = code;
    g_got_echo = false;
    g_echo_refusal = 0;
    if (!g_tx->writeValue(frame.data(), frame.size(), false)) {
      g_wait_code = 0;
      return "Bluetooth write failed";
    }
    pump(STEP_TIMEOUT_MS);
    g_wait_code = 0;
    if (g_got_echo) {
      const char *why = refusal_reason(g_echo_refusal);
      if (!why) return nullptr;
      // On a re-send, these mean the first copy was already taken.
      if (attempt > 0 && (g_echo_refusal & (0x800000 | 0x200000))) return nullptr;
      return why;
    }
    Serial.printf("[ble] no echo for %u (attempt %d)\n", code, attempt + 1);
  }
  return "No reply from machine";
}

// One-shot command, the way the official app sends them: wait for the echo,
// re-send only while the machine is asleep (it hasn't acted yet), never blindly.
// A missing echo from an awake machine is not treated as failure.
const char *send_command(const Bytes &frame) {
  if (!g_client || !g_client->isConnected() || !g_tx) return "Not connected";
  const uint16_t code = frame_code(frame);
  for (int attempt = 0; attempt < 3; ++attempt) {
    g_wait_code = code;
    g_got_echo = false;
    g_echo_refusal = 0;
    if (!g_tx->writeValue(frame.data(), frame.size(), false)) {
      g_wait_code = 0;
      return "Bluetooth write failed";
    }
    pump(1500);
    g_wait_code = 0;
    if (g_got_echo) return refusal_reason(g_echo_refusal);
    if (!g_sleeping) return nullptr;
  }
  return "Machine is asleep";
}

void run_cmd(const BleCmd &c) {
  const char *err = nullptr;
  switch (c.type) {
    case BleCmdType::Load: {
      if ((err = validate(c.recipe))) break;
      for (const Bytes &f : build_load_frames(c.recipe))
        if ((err = send_confirmed(f))) break;
      break;
    }
    case BleCmdType::Execute: err = send_confirmed(frame_execute()); break;
    case BleCmdType::Tare: err = send_command(frame_tare()); break;
    case BleCmdType::Pause: err = send_confirmed(frame_pause()); break;
    case BleCmdType::Resume: err = send_confirmed(frame_resume()); break;
    case BleCmdType::Stop: err = send_confirmed(frame_stop()); break;
    case BleCmdType::QuitRecipe: err = send_confirmed(frame_quit_recipe()); break;
    case BleCmdType::GrinderEnter: err = send_command(frame_grinder_enter(c.a, c.b)); break;
    case BleCmdType::GrinderStart: err = send_command(frame_grinder_start(c.a, c.b)); break;
    case BleCmdType::GrinderStop: err = send_command(frame_grinder_stop()); break;
    case BleCmdType::BrewerEnter: err = send_command(frame_brewer_enter(c.a, c.x)); break;
    case BleCmdType::BrewerStart: err = send_command(frame_brewer_start(c.x, c.y, c.z, c.a)); break;
    case BleCmdType::BrewerStop: err = send_command(frame_brewer_stop()); break;
    case BleCmdType::BrewerTemp: err = send_command(frame_brewer_temp(c.x)); break;
    case BleCmdType::BrewerPattern: err = send_command(frame_brewer_pattern(c.a)); break;
    case BleCmdType::ScaleEnter: err = send_command(frame_scale_enter()); break;
    case BleCmdType::QuitGrinder: err = send_command(frame_quit_grinder()); break;
    case BleCmdType::QuitBrewer: err = send_command(frame_quit_brewer()); break;
    case BleCmdType::QuitScale: err = send_command(frame_quit_scale()); break;
    case BleCmdType::WeightUnit: err = send_command(frame_weight_unit((WeightUnit)c.a)); break;
  }
  ui_msg(UiEvType::CmdResult, err, (int32_t)c.type, err == nullptr);
}

const NimBLEAdvertisedDevice *scan_for_xbloom() {
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(90);
  NimBLEScanResults res = scan->getResults(4000, false);
  for (int i = 0; i < res.getCount(); ++i) {
    const NimBLEAdvertisedDevice *d = res.getDevice(i);
    std::string n = d->getName();
    if (n.size() >= 6 && strncasecmp(n.c_str(), NAME_PREFIX, 6) == 0) return d;
  }
  return nullptr;
}

bool connect_to(const NimBLEAdvertisedDevice *dev) {
  if (!g_client) {
    g_client = NimBLEDevice::createClient();
    g_client->setConnectionParams(12, 24, 0, 400);
    g_client->setConnectTimeout(8000);
  }
  if (!g_client->connect(dev)) return false;

  g_tx = nullptr;
  NimBLERemoteCharacteristic *rx = nullptr;
  for (NimBLERemoteService *s : g_client->getServices(true)) {
    if (!g_tx) g_tx = s->getCharacteristic(NimBLEUUID(UUID_TX));
    if (!rx) rx = s->getCharacteristic(NimBLEUUID(UUID_RX));
  }
  if (!g_tx || !rx || !rx->canNotify() || !rx->subscribe(true, on_notify)) {
    Serial.println("[ble] FFE1/FFE2 not found or subscribe failed");
    g_client->disconnect();
    return false;
  }
  g_asm.reset();
  xQueueReset(q_raw);
  vTaskDelay(pdMS_TO_TICKS(300));  // let the notify stream settle before writing
  return true;
}

void ble_task(void *) {
  NimBLEDevice::init("xBloom Panel");
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(9);
  BleCmd cmd;
  for (;;) {
    if (!g_client || !g_client->isConnected()) {
      if (g_state == LinkState::Connected) Serial.println("[ble] disconnected");
      set_state(LinkState::Scanning);
      const NimBLEAdvertisedDevice *dev = scan_for_xbloom();
      if (!dev) {
        // drop any commands queued while offline, tell the UI why
        while (xQueueReceive(q_cmd, &cmd, 0))
          ui_msg(UiEvType::CmdResult, "Not connected", (int32_t)cmd.type, false);
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }
      std::string name = dev->getName();
      set_state(LinkState::Connecting, name.c_str());
      if (!connect_to(dev)) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        continue;
      }
      Serial.printf("[ble] connected to %s\n", name.c_str());
      set_state(LinkState::Connected, name.c_str());
    }
    if (xQueueReceive(q_cmd, &cmd, 0)) run_cmd(cmd);
    pump(30);
  }
}

}  // namespace

namespace ble_link {

void begin() {
  q_raw = xQueueCreate(16, sizeof(RawNotify));  // internal RAM is scarce; 16 is plenty
  q_cmd = xQueueCreate(8, sizeof(BleCmd));
  q_ui = xQueueCreate(64, sizeof(UiEvent));
  // core 0 (UI/LVGL runs on core 1); big stack for std::function + vectors
  xTaskCreatePinnedToCore(ble_task, "ble", 8192, nullptr, 3, nullptr, 0);
}

bool post(const BleCmd &c) { return xQueueSend(q_cmd, &c, 0) == pdTRUE; }
bool poll(UiEvent &e) { return xQueueReceive(q_ui, &e, 0) == pdTRUE; }
LinkState state() { return g_state; }

}  // namespace ble_link
