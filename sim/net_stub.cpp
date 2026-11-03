// Desktop preview stand-in for the Wi-Fi / clock / update module.
#include <string.h>
#include "panel_net.h"
namespace net {
static Wifi st = Wifi::Connected;
static net::Ap aps[] = {{"Home Wi-Fi", -48, true}, {"Home Wi-Fi 2.4G", -55, true},
                        {"Neighbour", -71, true}, {"Guest", -80, false}, {"Printer", -84, true}};
void begin() {}
Wifi state() { return st; }
const char *ssid() { return "Home Wi-Fi"; }
const char *ip() { return "192.168.1.50"; }
const char *hostname() { return "xbloom-panel.local"; }
bool has_saved_network() { return true; }
void join(const char *, const char *) {}
void forget() {}
void scan_start() {}
bool scan_running() { return false; }
int scan_results(Ap *out, int max) {
  int n = sizeof aps / sizeof aps[0];
  if (n > max) n = max;
  memcpy(out, aps, n * sizeof(Ap));
  return n;
}
bool time_valid() { return true; }
bool local_time(struct tm *t) {
  memset(t, 0, sizeof *t);
  t->tm_year = 126; t->tm_mon = 9; t->tm_mday = 29; t->tm_wday = 4; t->tm_hour = 9; t->tm_min = 25; t->tm_mon = 10; t->tm_mday = 2; t->tm_wday = 1;
  return true;
}
static Upd upd = Upd::Idle;
void update_page(bool) {}
Upd update_state() { return upd; }
int update_progress() { return 45; }
const char *update_error() { return ""; }
bool pref_clock() { return true; }
void set_pref_clock(bool) {}
bool pref_night_off() { return false; }
void set_pref_night_off(bool) {}
static int sleep_m = 3, ccolor = 0;
int pref_sleep_min() { return sleep_m; }
void set_pref_sleep_min(int m) { sleep_m = m; }
int pref_clock_color() { return ccolor; }
void set_pref_clock_color(int c) { ccolor = c; }
}  // namespace net
