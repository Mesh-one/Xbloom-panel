// Wi-Fi, clock (NTP) and the over-the-air update web page.
// Runs its own FreeRTOS task, so an update upload never freezes the screen.
// Everything here is safe to call from the UI loop.
#pragma once
#include <stdint.h>
#include <time.h>

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

namespace net {

enum class Wifi : uint8_t { Off, Connecting, Connected, WrongPassword, NotFound };

void begin();                 // load saved settings, join the saved network
Wifi state();
const char *ssid();           // network joined / being joined ("" if none)
const char *ip();             // "" until connected
const char *hostname();       // mDNS name, e.g. "xbloom-panel.local"
bool has_saved_network();
void join(const char *ssid, const char *pass);  // saves on success attempt
void forget();

// Network scan (for the Wi-Fi screen)
struct Ap { char ssid[33]; int8_t rssi; bool secure; };
void scan_start();
bool scan_running();
int scan_results(Ap *out, int max);   // strongest first, no duplicates

// Clock
bool time_valid();
bool local_time(struct tm *out);      // Riyadh time (UTC+3)

// Update page: only accepts uploads while enabled (Settings > Software update is open)
enum class Upd : uint8_t { Idle, Receiving, Done, Failed };
void update_page(bool enabled);
Upd update_state();
int update_progress();                // 0..100
const char *update_error();

// Saved panel settings
bool pref_clock();                    // show a clock instead of a black screen
void set_pref_clock(bool on);
bool pref_night_off();                // screen fully off 11 PM - 6 AM
void set_pref_night_off(bool on);
int pref_sleep_min();                // screen off (or clock) after this many idle minutes; 0 = never
void set_pref_sleep_min(int m);
int pref_clock_color();              // 0 white, 1 blue, 2 brown
void set_pref_clock_color(int c);

}  // namespace net
