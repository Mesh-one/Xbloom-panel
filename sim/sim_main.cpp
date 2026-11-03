// Desktop render of the UI for a layout check (no hardware). Writes PPM frames.
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <deque>
#include "ui.h"
#include "ble_link.h"
static uint32_t now = 100000;
uint32_t millis() { return now; }
static std::deque<UiEvent> evq;
namespace ble_link {
void begin() {}
bool post(const BleCmd &) { return true; }
bool poll(UiEvent &e) { if (evq.empty()) return false; e = evq.front(); evq.pop_front(); return true; }
LinkState state() { return LinkState::Connected; }
}
static uint16_t fb[800 * 480];
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  for (int y = a->y1; y <= a->y2; ++y)
    memcpy(&fb[y * 800 + a->x1], px + ((y - a->y1) * (a->x2 - a->x1 + 1)) * 2, (a->x2 - a->x1 + 1) * 2);
  lv_display_flush_ready(d);
}
static void save(const char *fn) {
  FILE *f = fopen(fn, "wb"); fprintf(f, "P6 800 480 255\n");
  for (int i = 0; i < 800 * 480; ++i) { uint16_t c = fb[i];
    unsigned char rgb[3] = {(unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2), (unsigned char)((c & 31) << 3)};
    fwrite(rgb, 1, 3, f); }
  fclose(f);
}
static void run(int ms) { for (int t = 0; t < ms; t += 10) { now += 10; ui::tick(); lv_timer_handler(); } }
static void ev(UiEvType t, int i = 0, float f = 0, const char *m = "") {
  UiEvent e; e.type = t; e.i = i; e.f = f; strncpy(e.msg, m, sizeof e.msg - 1); evq.push_back(e);
}
static void frames(const char *prefix, int ms, int every) {
  char fn[128];
  for (int t = 0, k = 0; t <= ms; t += every, ++k) { run(every); snprintf(fn, sizeof fn, "%s_%03d.ppm", prefix, k); save(fn); }
}
int main(int argc, char **argv) {
  lv_init(); lv_tick_set_cb(millis);
  static uint8_t buf[800 * 480 * 2];
  lv_display_t *d = lv_display_create(800, 480);
  lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(d, flush);
  lv_display_set_buffers(d, buf, nullptr, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
  ui::begin(); run(300); save("0_connecting.ppm");
  if (argc > 1 && !strcmp(argv[1], "detail")) {
    ev(UiEvType::Link, 3, 0, "XBLOOM 381"); run(500);
    ui::sim_show(1, 0); run(800); save("d0_recipes.ppm");
    ui::sim_show(2, 0); run(800); save("d1_detail.ppm");
    ui::sim_show(7, 0); run(600); save("d2_ready.ppm"); ui::sim_show(8, 0);
    ui::sim_show(21, 0);
    for (int k = 0; k < 25; ++k) { ev(UiEvType::Water, 0, 0); run(40); }
    save("d3_brewing_start.ppm");
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "anim")) {
    ev(UiEvType::Link, 3, 0, "XBLOOM 381"); run(500);
    ui::sim_show(21, 0);
    {  // a compressed brew: 3 pours (60 / 80 / 80 ml) with pauses between them
      char fn[64]; int k = 0;
      auto water_at = [](float s) -> float {
        if (s < 0.6f) return 0; if (s < 2.6f) return (s - 0.6f) / 2 * 60; if (s < 3.6f) return 60;
        if (s < 5.6f) return 60 + (s - 3.6f) / 2 * 80; if (s < 6.4f) return 140;
        if (s < 8.4f) return 140 + (s - 6.4f) / 2 * 80; return 220; };
      for (float s = 0; s <= 9.2f; s += 0.04f, ++k) {
        const float w = water_at(s);
        ev(UiEvType::Water, 0, w); ev(UiEvType::Weight, 0, w * 0.64f + 22 * (s > 0.8f));
        if (s > 0.55f && s < 0.6f) ev(UiEvType::PourIndex, 0);
        if (s > 3.55f && s < 3.6f) ev(UiEvType::PourIndex, 1);
        if (s > 6.35f && s < 6.4f) ev(UiEvType::PourIndex, 2);
        run(40); snprintf(fn, sizeof fn, "brew_%03d.ppm", k); save(fn);
      }
    }
    ui::sim_show(26, 0); ui::sim_show(25, 0); run(400); save("stats.ppm");
    ui::sim_show(22, 0); frames("finish", 3600, 40);
    ui::sim_show(0, 0); ui::sim_show(24, 0); run(200); save("clock_blue.ppm");
    ui::sim_show(23, 0); run(200); save("clock_white.ppm");
    return 0;
  }
  ev(UiEvType::Link, 3, 0, "XBLOOM 381"); run(1500); save("1_home.ppm");
  ui::sim_show(1, 0); run(1500); save("2_recipes.ppm");
  ui::sim_show(2, 0); run(1500); save("3_detail_mad.ppm");
  ui::sim_show(7, 0); run(600); save("3b_modal.ppm"); ui::sim_show(8, 0);
  ui::sim_show(2, 2); run(1500); save("4_detail_starter.ppm");
  ui::sim_show(3, 1); ev(UiEvType::Weight, 0, 88.4f); run(1500); save("5_detail_brewing.ppm");
  ui::sim_show(4, 0); run(1500); save("6_grinder.ppm");
  ui::sim_show(5, 0); run(1500); save("7_brewer.ppm");
  ev(UiEvType::Weight, 0, 0.0f); ui::sim_show(6, 0); run(1500); save("8_scale.ppm");
  ui::sim_show(9, 0); run(1500); save("9_settings.ppm");
  ui::sim_show(10, 0); run(1500); save("10_wifi.ppm");
  ui::sim_show(11, 0); run(1500); save("11_password.ppm");
  ui::sim_show(12, 0); run(1500); save("12_update.ppm");
  ui::sim_show(13, 0); run(1500); save("13_clock.ppm");
  ui::sim_show(14, 0); ui::sim_show(0, 0); run(1500); save("14_home_synced.ppm");
  ui::sim_show(18, 0); run(1500); save("15_created.ppm");
  ui::sim_show(17, 0); run(1500); save("16_shared.ppm");
  ui::sim_show(2, 0); run(1500); save("17_detail_heart.ppm");
  ui::sim_show(15, 0); run(1500); save("18_editor.ppm");
  ui::sim_show(19, 0); run(600); save("19_confirm.ppm"); ui::sim_show(20, 0);
  ui::sim_show(16, 0); run(1500); save("20_account.ppm");
}
