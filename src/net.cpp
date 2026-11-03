#include "panel_net.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

namespace {

const char *HOST = "xbloom-panel";
const char *TZ_RIYADH = "<+03>-3";  // POSIX TZ: UTC+3, no daylight saving

Preferences prefs;
SemaphoreHandle_t mtx;
WebServer server(80);

// shared state (guarded by mtx where it's more than one word)
volatile net::Wifi g_state = net::Wifi::Off;
char g_ssid[33] = "", g_pass[65] = "", g_ip[16] = "", g_host[40] = "";
volatile bool g_join_req = false, g_forget_req = false, g_scan_req = false, g_scan_running = false;
volatile uint8_t g_disc_reason = 0;
uint32_t g_connect_started = 0;
net::Ap g_aps[24];
int g_n_aps = 0;
bool g_pref_clock = true, g_pref_night = false;
int g_pref_sleep = 3, g_pref_ccolor = 0;
volatile bool g_server_started = false, g_mdns = false, g_ntp = false;

volatile bool g_page_enabled = false;
volatile net::Upd g_upd = net::Upd::Idle;
volatile int g_upd_pct = 0;
char g_upd_err[64] = "";
size_t g_upd_total = 0, g_upd_written = 0;
uint8_t g_upd_probe[2];

struct Lock {
  Lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mtx); }
};

void set_err(const char *e) {
  Lock l;
  strlcpy(g_upd_err, e, sizeof g_upd_err);
}

// ---------------------------------------------------------------------------
// update web page
// ---------------------------------------------------------------------------
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>xBloom Panel update</title>
<style>
:root{color-scheme:dark}body{margin:0;background:#000;color:#fff;font:16px -apple-system,system-ui,sans-serif}
main{max-width:440px;margin:0 auto;padding:32px 20px}h1{font-size:26px;margin:0 0 4px}.sub{color:#8e8e93;margin:0 0 28px}
.card{background:#1c1c1e;border-radius:16px;padding:20px}label{display:block;color:#aeaeb2;margin-bottom:10px}
input[type=file]{width:100%;color:#fff}button{margin-top:20px;width:100%;border:0;border-radius:28px;padding:16px;
font-size:17px;font-weight:600;background:#fff;color:#000}button:disabled{opacity:.4}
.bar{height:8px;background:#2c2c2e;border-radius:4px;margin-top:20px;overflow:hidden}.bar i{display:block;height:100%;width:0;background:#34c759}
#msg{margin-top:14px;min-height:22px;color:#aeaeb2}.err{color:#ff6b6b!important}.ok{color:#34c759!important}
</style></head><body><main><h1>xBloom Panel</h1><p class="sub">Installed version %VER%</p>
<div class="card"><label for="f">Choose the update file (<b>xbloom-panel-update.bin</b>)</label>
<input id="f" type="file" accept=".bin"><button id="b" disabled>Install update</button>
<div class="bar"><i id="p"></i></div><div id="msg"></div></div></main><script>
const f=document.getElementById('f'),b=document.getElementById('b'),p=document.getElementById('p'),m=document.getElementById('msg');
f.onchange=()=>{b.disabled=!f.files.length;m.textContent='';m.className=''};
b.onclick=()=>{const x=new XMLHttpRequest(),d=new FormData();d.append('fw',f.files[0]);b.disabled=f.disabled=true;
m.className='';m.textContent='Uploading...';x.upload.onprogress=e=>{if(e.lengthComputable)p.style.width=(e.loaded/e.total*100)+'%'};
x.onload=()=>{let r={};try{r=JSON.parse(x.responseText)}catch(e){}
if(x.status==200&&r.ok){p.style.width='100%';m.className='ok';m.textContent='Installed. The panel is restarting.'}
else{m.className='err';m.textContent=r.error||('Failed ('+x.status+')');b.disabled=f.disabled=false}};
x.onerror=()=>{m.className='err';m.textContent='Connection lost. Try again.';b.disabled=f.disabled=false};
x.open('POST','/update');x.send(d)};
</script></body></html>)HTML";

void send_json(int code, bool ok, const char *err = "") {
  String body = ok ? String("{\"ok\":true}") : String("{\"ok\":false,\"error\":\"") + err + "\"}";
  server.send(code, "application/json", body);
}

void handle_root() {
  if (!g_page_enabled) {
    server.send(403, "text/plain", "Open Settings > Software update on the panel first, then reload this page.");
    return;
  }
  String page = FPSTR(PAGE);
  page.replace("%VER%", FW_VERSION);
  server.send(200, "text/html", page);
}

void handle_upload() {
  HTTPUpload &up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      g_upd_written = 0;
      g_upd_probe[0] = g_upd_probe[1] = 0;
      g_upd_total = server.header("Content-Length").toInt();
      set_err("");
      if (!g_page_enabled) { set_err("Open Settings > Software update on the panel first"); g_upd = net::Upd::Failed; return; }
      g_upd_pct = 0;
      g_upd = net::Upd::Receiving;
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        set_err("Not enough space for the update");
        g_upd = net::Upd::Failed;
      }
      break;
    case UPLOAD_FILE_WRITE:
      if (g_upd != net::Upd::Receiving) return;
      // The USB file (firmware.factory.bin) starts with the bootloader and has the
      // partition table (magic AA 50) at 0x8000 - it must never go into an app slot.
      for (size_t i = 0; i < up.currentSize; ++i) {
        size_t off = g_upd_written + i;
        if (off == 0x8000 || off == 0x8001) g_upd_probe[off - 0x8000] = up.buf[i];
      }
      if (g_upd_written + up.currentSize > 0x8001 && g_upd_probe[0] == 0xAA && g_upd_probe[1] == 0x50) {
        Update.abort();
        set_err("That's the USB file. Use xbloom-panel-update.bin");
        g_upd = net::Upd::Failed;
        return;
      }
      if (Update.write(up.buf, up.currentSize) != up.currentSize) {
        set_err(Update.errorString());
        Update.abort();
        g_upd = net::Upd::Failed;
        return;
      }
      g_upd_written += up.currentSize;
      if (g_upd_total > 0) g_upd_pct = (int)min<size_t>(99, g_upd_written * 100 / g_upd_total);
      break;
    case UPLOAD_FILE_END:
      if (g_upd != net::Upd::Receiving) return;
      if (Update.end(true)) {
        g_upd_pct = 100;
        g_upd = net::Upd::Done;
      } else {
        set_err(Update.errorString());
        g_upd = net::Upd::Failed;
      }
      break;
    case UPLOAD_FILE_ABORTED:
      Update.abort();
      set_err("Upload interrupted");
      g_upd = net::Upd::Failed;
      break;
  }
}

void handle_update_done() {
  if (g_upd == net::Upd::Done) {
    send_json(200, true);
    delay(1500);  // let the reply reach the phone and the panel show "restarting"
    ESP.restart();
  } else {
    char err[64];
    {
      Lock l;
      strlcpy(err, g_upd_err[0] ? g_upd_err : "Update failed", sizeof err);
    }
    send_json(400, false, err);
  }
}

void start_services() {
  if (!g_server_started) {
    const char *hdrs[] = {"Content-Length"};
    server.collectHeaders(hdrs, 1);
    server.on("/", HTTP_GET, handle_root);
    server.on("/update", HTTP_POST, handle_update_done, handle_upload);
    server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
    server.begin();
    g_server_started = true;
  }
  if (!g_mdns && MDNS.begin(HOST)) {
    MDNS.addService("http", "tcp", 80);
    g_mdns = true;
  }
  if (!g_ntp) {
    configTzTime(TZ_RIYADH, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    g_ntp = true;
  }
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------
void on_wifi_event(WiFiEvent_t ev, WiFiEventInfo_t info) {
  if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) g_disc_reason = info.wifi_sta_disconnected.reason;
  if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) g_disc_reason = 0;
}

void do_join() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOST);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(false, false);
  char s[33], p[65];
  {
    Lock l;
    strlcpy(s, g_ssid, sizeof s);
    strlcpy(p, g_pass, sizeof p);
  }
  g_disc_reason = 0;
  g_connect_started = millis();
  g_state = net::Wifi::Connecting;
  WiFi.begin(s, p[0] ? p : nullptr);
}

void do_scan() {
  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
  g_scan_running = true;
  int n = WiFi.scanNetworks(false, false);
  Lock l;
  g_n_aps = 0;
  for (int i = 0; i < n && g_n_aps < 24; ++i) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (int k = 0; k < g_n_aps; ++k)
      if (s == g_aps[k].ssid) { dup = true; break; }
    if (dup) continue;  // results come strongest first, keep that one
    strlcpy(g_aps[g_n_aps].ssid, s.c_str(), sizeof g_aps[0].ssid);
    g_aps[g_n_aps].rssi = (int8_t)WiFi.RSSI(i);
    g_aps[g_n_aps].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    ++g_n_aps;
  }
  WiFi.scanDelete();
  g_scan_running = false;
}

void update_state() {
  if (g_state == net::Wifi::Off) return;
  if (WiFi.status() == WL_CONNECTED) {
    if (g_state != net::Wifi::Connected) {
      Lock l;
      strlcpy(g_ip, WiFi.localIP().toString().c_str(), sizeof g_ip);
      g_state = net::Wifi::Connected;
      Serial.printf("[wifi] connected, %s\n", g_ip);
    }
    start_services();
    return;
  }
  if (g_state == net::Wifi::Connected) {  // dropped; auto-reconnect keeps trying
    Lock l;
    g_ip[0] = 0;
    g_state = net::Wifi::Connecting;
    g_connect_started = millis();
  }
  const uint8_t r = g_disc_reason;
  const bool auth_fail = r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                         r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_MIC_FAILURE ||
                         r == WIFI_REASON_AUTH_EXPIRE;
  if (g_state == net::Wifi::Connecting && millis() - g_connect_started > 12000) {
    if (auth_fail) g_state = net::Wifi::WrongPassword;
    else if (r == WIFI_REASON_NO_AP_FOUND) g_state = net::Wifi::NotFound;
  }
}

void net_task(void *) {
  for (;;) {
    if (g_forget_req) {
      g_forget_req = false;
      WiFi.disconnect(true, true);
      WiFi.mode(WIFI_OFF);
      {
        Lock l;
        g_ssid[0] = g_pass[0] = g_ip[0] = 0;
      }
      prefs.remove("ssid");
      prefs.remove("pass");
      g_state = net::Wifi::Off;
    }
    if (g_join_req) {
      g_join_req = false;
      do_join();
    }
    if (g_scan_req) {
      g_scan_req = false;
      do_scan();
    }
    update_state();
    if (g_server_started) server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(g_upd == net::Upd::Receiving ? 1 : 20));
  }
}

}  // namespace

namespace net {

void begin() {
  mtx = xSemaphoreCreateMutex();
  prefs.begin("panel", false);
  g_pref_clock = prefs.getBool("clock", true);
  g_pref_night = prefs.getBool("night", false);
  g_pref_sleep = prefs.getInt("sleep", 3);
  g_pref_ccolor = prefs.getInt("ccolor", 0);
  String s = prefs.getString("ssid", ""), p = prefs.getString("pass", "");
  strlcpy(g_ssid, s.c_str(), sizeof g_ssid);
  strlcpy(g_pass, p.c_str(), sizeof g_pass);
  snprintf(g_host, sizeof g_host, "%s.local", HOST);
  WiFi.onEvent(on_wifi_event);
  if (g_ssid[0]) g_join_req = true;
  else WiFi.mode(WIFI_OFF);
  xTaskCreatePinnedToCore(net_task, "net", 8192, nullptr, 2, nullptr, 0);
}

Wifi state() { return g_state; }
const char *ssid() { return g_ssid; }
const char *ip() { return g_ip; }
const char *hostname() { return g_host; }
bool has_saved_network() { return g_ssid[0] != 0; }

void join(const char *s, const char *p) {
  {
    Lock l;
    strlcpy(g_ssid, s, sizeof g_ssid);
    strlcpy(g_pass, p ? p : "", sizeof g_pass);
  }
  prefs.putString("ssid", s);
  prefs.putString("pass", p ? p : "");
  g_state = Wifi::Connecting;
  g_join_req = true;
}

void forget() { g_forget_req = true; }

void scan_start() {
  g_scan_running = true;
  g_scan_req = true;
}
bool scan_running() { return g_scan_running; }
int scan_results(Ap *out, int max) {
  Lock l;
  int n = min(max, g_n_aps);
  memcpy(out, g_aps, n * sizeof(Ap));
  return n;
}

bool time_valid() {
  time_t now = time(nullptr);
  return now > 1735689600;  // after 2025-01-01: NTP has answered
}
bool local_time(struct tm *out) {
  if (!time_valid()) return false;
  time_t now = time(nullptr);
  localtime_r(&now, out);
  return true;
}

void update_page(bool enabled) { g_page_enabled = enabled; if (enabled && g_upd != Upd::Receiving) g_upd = Upd::Idle; }
Upd update_state() { return g_upd; }
int update_progress() { return g_upd_pct; }
const char *update_error() { return g_upd_err; }

bool pref_clock() { return g_pref_clock; }
void set_pref_clock(bool on) { g_pref_clock = on; prefs.putBool("clock", on); }
bool pref_night_off() { return g_pref_night; }
void set_pref_night_off(bool on) { g_pref_night = on; prefs.putBool("night", on); }
int pref_sleep_min() { return g_pref_sleep; }
void set_pref_sleep_min(int m) { g_pref_sleep = m; prefs.putInt("sleep", m); }
int pref_clock_color() { return g_pref_ccolor; }
void set_pref_clock_color(int c) { g_pref_ccolor = c; prefs.putInt("ccolor", c); }

}  // namespace net
