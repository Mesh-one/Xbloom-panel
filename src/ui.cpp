// Panel UI (LVGL 9.1, 800x480) laid out after the xBloom app:
// Home (Grinder / Brewer / Scale + Favorite Recipes), My Recipes, Recipe detail,
// and the Grinder, Brewer and Scale screens. All LVGL calls run on the loop task.
//
// Layout: one spacing system (ui_theme.h) — 36 px status bar, 24 px margins,
// 16 px gaps, 20 px card corners — and numbers aligned on their baselines.
#include "ui.h"

#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ble_link.h"
#include "cloud.h"
#include "panel_storage.h"
#include "recipes.h"
#include "panel_net.h"
#include "ui_theme.h"
#include "dotfx.h"

using th::hex;
using namespace th;

namespace {

// ============================================================================
// state
// ============================================================================
enum class Flow { Idle, Loading, Loaded, Starting, Brewing, Paused };

std::vector<PanelRecipe> lib;  // what the panel shows (see library.h); screens refer to recipes by uid
uint32_t next_uid = 1;
uint32_t cur = 0;              // recipe shown on the detail screen (uid)
uint32_t active = 0;           // recipe loaded/brewing on the machine (uid), 0 = none
xb::Recipe active_copy;        // what was sent to the machine
Flow flow = Flow::Idle;
LinkState link = LinkState::Idle;
char machine_name[32] = "XBLOOM";
float weight_g = 0, water_ml = 0;
float wake_ref_weight = 0;   // weight when we last woke the screen
bool wake_req = false;
int pour_idx = -1;
uint32_t brew_start_ms = 0;
uint32_t intro_for = 0;       // the recipe the brewing scene was opened for

// manual modules
int g_size = 36, g_rpm = 90;
bool grinding = false;
int b_volume = 250, b_temp = 85;  // temp in display °C (39 = RT, 96 = BP)
float b_flow = 3.5f;
uint8_t b_pattern = xb::PAT_CIRCULAR;
bool pouring = false;
uint32_t pour_start_ms = 0;
int unit = 0;  // scale: 0 g, 1 oz, 2 ml
bool sw_running = false;
uint32_t sw_start = 0, sw_elapsed = 0;

// screens
lv_obj_t *scr_home, *scr_detail, *scr_grinder, *scr_brewer, *scr_scale;
lv_obj_t *page_home, *page_recipes, *page_settings, *tab_lbl[3];
lv_obj_t *home_name, *home_chev, *home_status, *home_spin, *home_dot, *fav_row;
lv_obj_t *seg_pill, *seg_lbl[3], *rec_list;
int seg = 0;
lv_obj_t *sb_bt, *sb_wifi, *sb_clock;

// detail
lv_obj_t *d_stats[3][2], *d_stat_unit[3], *d_start, *d_start_icon, *d_start_lbl, *d_stop, *modal;
struct Col { lv_obj_t *bar, *fill; double vol, cum; } d_cols[xb::MAX_POURS];
int d_ncols = 0;

// module screens
lv_obj_t *gr_weight, *gr_play_icon, *gr_use, *br_weight, *br_timer, *br_play_icon;
lv_obj_t *sc_value, *sc_unit, *sc_timer, *sc_play_icon;
lv_obj_t *pat_btn[3], *unit_btn[3];
lv_obj_t *toast, *toast_lbl;
lv_timer_t *toast_timer;

// ============================================================================
// small helpers
// ============================================================================
// Only touch LVGL when something really changes: every change makes the panel
// redraw that area, and redraws compete with the display for PSRAM bandwidth
// (redrawing the clock and status bar 5x a second caused the line glitches).
void set_text(lv_obj_t *l, const char *t) {
  const char *cur = lv_label_get_text(l);
  if (cur && t && strcmp(cur, t) == 0) return;
  lv_label_set_text(l, t);
}
void set_text_color(lv_obj_t *o, lv_color_t c, lv_style_selector_t sel) {
  if (sel == 0 && lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c)) return;
  lv_obj_set_style_text_color(o, c, sel);
}
void set_opa(lv_obj_t *o, lv_opa_t v) {
  if (lv_obj_get_style_opa(o, LV_PART_MAIN) == v) return;
  lv_obj_set_style_opa(o, v, 0);
}

lv_color_t darker(uint32_t c, lv_opa_t amt) { return lv_color_darken(hex(c), amt); }

PanelRecipe *find(uint32_t uid) {
  for (auto &p : lib)
    if (p.uid == uid) return &p;
  return nullptr;
}
// Recipes tab segment: 0 Created, 1 Shared, 2 Starter
int seg_of(const PanelRecipe &p) { return p.kind == KIND_SHARED ? 1 : p.kind == KIND_STARTER ? 2 : 0; }

lv_obj_t *pill(lv_obj_t *parent, int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg,
               lv_event_cb_t cb);
void open_editor(uint32_t uid);
void open_account(lv_event_t * = nullptr);
void toggle_favorite(uint32_t uid);
lv_obj_t *st_acc_val;

// Plain rectangle. Not clickable unless on_click() is called on it, so
// decorations never swallow a tap meant for the card underneath.
lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, int radius) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, hex(bg), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_clip_corner(o, true, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

lv_obj_t *group(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

lv_obj_t *text(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c) {
  lv_obj_t *l = lv_label_create(parent);
  set_text(l, t);
  lv_obj_set_style_text_font(l, f, 0);
  set_text_color(l, c, 0);
  return l;
}

// Align a label so its text baseline sits `from_bottom` px above the parent's bottom edge.
void baseline_align(lv_obj_t *l, lv_align_t align, int x, int from_bottom) {
  const lv_font_t *f = lv_obj_get_style_text_font(l, 0);
  lv_obj_align(l, align, x, f->base_line - from_bottom);
}

// Put `l` to the left (or right) of `ref` with the two baselines lined up.
void baseline_next_to(lv_obj_t *l, lv_obj_t *ref, bool left, int gap) {
  const lv_font_t *fl = lv_obj_get_style_text_font(l, 0);
  const lv_font_t *fr = lv_obj_get_style_text_font(ref, 0);
  lv_obj_update_layout(ref);
  lv_obj_align_to(l, ref, left ? LV_ALIGN_OUT_LEFT_BOTTOM : LV_ALIGN_OUT_RIGHT_BOTTOM, left ? -gap : gap,
                  fl->base_line - fr->base_line);
}

void on_click(lv_obj_t *o, lv_event_cb_t cb, void *user = nullptr) {
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, user);
}

// Subtle press feedback for anything tappable.
void pressable(lv_obj_t *o) {
  lv_obj_set_style_opa(o, LV_OPA_70, LV_STATE_PRESSED);
}

void show(lv_obj_t *o, bool on) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == !on) return;
  if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void post(BleCmdType t, int a = 0, int b = 0, double x = 0, double y = 0, double z = 0) {
  BleCmd c;
  c.type = t;
  c.a = a; c.b = b; c.x = x; c.y = y; c.z = z;
  ble_link::post(c);
}

void fmt_time(char *buf, size_t n, uint32_t ms) {
  uint32_t s = ms / 1000;
  snprintf(buf, n, "%02lu'%02lu''", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

void show_toast(const char *msg) {
  set_text(toast_lbl, msg);
  show(toast, true);
  lv_obj_move_foreground(toast);
  lv_timer_reset(toast_timer);
  lv_timer_resume(toast_timer);
  wake_req = true;
}

lv_obj_t *new_screen(uint32_t bg) {
  lv_obj_t *s = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s, hex(bg), 0);
  lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

// Screen transitions: recipes slide in from the right, modules rise from the bottom.
enum class Anim { None, PushLeft, PopRight, RiseUp, DropDown };
void load(lv_obj_t *scr, Anim a) {
  lv_screen_load_anim_t t = LV_SCR_LOAD_ANIM_NONE;
  switch (a) {
    case Anim::PushLeft: t = LV_SCR_LOAD_ANIM_OVER_LEFT; break;
    case Anim::PopRight: t = LV_SCR_LOAD_ANIM_OUT_RIGHT; break;
    case Anim::RiseUp: t = LV_SCR_LOAD_ANIM_OVER_TOP; break;
    case Anim::DropDown: t = LV_SCR_LOAD_ANIM_OUT_BOTTOM; break;
    default: break;
  }
  // Full-screen slides redraw all 384,000 pixels every frame, which this panel's
  // software renderer can't do smoothly, so screens switch instantly.
  (void)t;
  lv_screen_load(scr);
}

// Dot-grid module icons (grinder: staggered, brewer: drop, scale: square).
enum class Dots { Grinder, Brewer, Scale };
lv_obj_t *dot_icon(lv_obj_t *parent, Dots kind, lv_color_t col, int d, int gap) {
  static const char *grinder[] = {"1.1.1", ".1.1.", "1.1.1", ".1.1.", "1.1.1"};
  static const char *brewer[] = {"..1..", ".111.", "11111", "11111", ".111."};
  static const char *scale[] = {"11111", "11111", "11111", "11111", "11111"};
  const char **m = kind == Dots::Grinder ? grinder : kind == Dots::Brewer ? brewer : scale;
  const int step = d + gap, size = 5 * step - gap;
  lv_obj_t *c = group(parent, 0, 0, size, size);
  for (int r = 0; r < 5; ++r)
    for (int k = 0; k < 5; ++k)
      if (m[r][k] == '1') {
        lv_obj_t *dot = box(c, k * step, r * step, d, d, 0, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(dot, col, 0);
      }
  return c;
}

lv_obj_t *ring(lv_obj_t *parent, int size, int w, lv_color_t col, int a0 = 0, int a1 = 360) {
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_set_size(a, size, size);
  lv_obj_remove_style(a, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, w, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(a, col, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(a, false, LV_PART_INDICATOR);
  lv_obj_set_style_pad_all(a, 0, 0);
  lv_arc_set_bg_angles(a, 0, 360);
  lv_arc_set_angles(a, a0, a1);
  return a;
}

// Pour-pattern icon: centered = dot in a ring, circular = broken ring, spiral = nested rings.
lv_obj_t *pattern_icon(lv_obj_t *parent, uint8_t pat, int size, lv_color_t col) {
  lv_obj_t *c = group(parent, 0, 0, size, size);
  const int w = size > 24 ? 4 : 2;
  if (pat == xb::PAT_CENTERED) {
    lv_obj_center(ring(c, size, w, lv_color_darken(col, 120)));
    lv_obj_t *dot = box(c, 0, 0, size / 2, size / 2, 0, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(dot, col, 0);
    lv_obj_center(dot);
  } else if (pat == xb::PAT_CIRCULAR) {
    for (int k = 0; k < 4; ++k) lv_obj_center(ring(c, size, w, col, k * 90 + 12, k * 90 + 78));
  } else {
    lv_obj_center(ring(c, size, w, col, 0, 300));
    lv_obj_center(ring(c, size * 6 / 10, w, col, 60, 360));
    lv_obj_t *dot = box(c, 0, 0, w + 2, w + 2, 0, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(dot, col, 0);
    lv_obj_center(dot);
  }
  return c;
}

// Small cup glyph: Omni = squat cup, others = dripper cone.
lv_obj_t *cup_icon(lv_obj_t *parent, int cup, lv_color_t col) {
  lv_obj_t *c = group(parent, 0, 0, 22, 20);
  if (cup == recipes::CUP_OMNI) {
    lv_obj_set_style_bg_color(box(c, 0, 4, 22, 4, 0, 2), col, 0);
    lv_obj_set_style_bg_color(box(c, 3, 0, 16, 18, 0, 5), col, 0);
  } else {
    static lv_point_precise_t pts[] = {{2, 2}, {20, 2}, {11, 18}, {2, 2}};
    lv_obj_t *l = lv_line_create(c);
    lv_line_set_points(l, pts, 4);
    lv_obj_set_style_line_width(l, 5, 0);
    lv_obj_set_style_line_color(l, col, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
  }
  return c;
}


// Heart: a diamond with two circles on its upper edges.
lv_obj_t *heart_icon(lv_obj_t *parent, uint32_t col) {
  lv_obj_t *c = group(parent, 0, 0, 24, 22);
  lv_obj_t *sq = box(c, 6, 6, 12, 12, col, 1);
  lv_obj_set_style_transform_rotation(sq, 450, 0);
  lv_obj_set_style_transform_pivot_x(sq, 6, 0);
  lv_obj_set_style_transform_pivot_y(sq, 6, 0);
  lv_obj_set_style_clip_corner(sq, false, 0);
  box(c, 1, 2, 12, 12, col, LV_RADIUS_CIRCLE);
  box(c, 11, 2, 12, 12, col, LV_RADIUS_CIRCLE);
  return c;
}
void heart_color(lv_obj_t *h, uint32_t col) {
  for (uint32_t i = 0; i < lv_obj_get_child_count(h); ++i) lv_obj_set_style_bg_color(lv_obj_get_child(h, i), hex(col), 0);
}

double ratio_of(const xb::Recipe &r) { return round(r.total_water() / r.dose_g * 10) / 10; }

void recipe_line(const xb::Recipe &r, char *buf, size_t n) {
  snprintf(buf, n, "1:%g-%.0fml   |   %gg   |   %d Pours", ratio_of(r), r.total_water(), r.dose_g, r.n_pours);
}

// Pastel recipe tile: label top-left, cup bottom-left, big pour count bleeding
// off the bottom-right corner (like the app).
lv_obj_t *recipe_tile(lv_obj_t *parent, int x, int y, int w, int h, const PanelRecipe &p, bool show_name) {
  const uint32_t bg = p.color;
  lv_color_t ink = darker(bg, 70);
  lv_obj_t *t = box(parent, x, y, w, h, bg, R_SM);
  char n[4];
  snprintf(n, sizeof n, "%d", p.r.n_pours);
  lv_obj_t *num = text(t, n, &inter_64b, lv_color_darken(hex(bg), 36));
  baseline_align(num, LV_ALIGN_BOTTOM_RIGHT, -6, h < 90 ? -4 : 2);  // small tiles: number bleeds off the edge like the app
  lv_obj_t *top = text(t, show_name ? p.r.name : recipes::cup_name(p.r.cup_type),
                       show_name ? &inter_20b : &inter_16, ink);
  lv_label_set_long_mode(top, LV_LABEL_LONG_DOT);
  lv_obj_set_width(top, w - 24);
  if (show_name) lv_obj_set_height(top, 2 * lv_font_get_line_height(&inter_20b));
  lv_obj_set_pos(top, 12, 10);
  lv_obj_t *cup = cup_icon(t, p.r.cup_type, ink);
  lv_obj_align(cup, LV_ALIGN_BOTTOM_LEFT, 12, -12);
  lv_obj_set_user_data(t, (void *)(uintptr_t)p.uid);
  return t;
}

// ============================================================================
// value card with a drag-to-set fill (Grinder / Brewer screens)
// Label top-left; value and unit bottom-right on one baseline.
// ============================================================================
struct ValueCard {
  lv_obj_t *card, *fill, *value, *unit;
  double min, max, step, v;
  const char *fmt;
  void (*changed)(double v, bool released);
};
ValueCard cards[5];
int n_cards = 0;
const int CARD_BASELINE = 18;  // value baseline, px above the card's bottom edge

void card_render(ValueCard &c) {
  const int inner = lv_obj_get_width(c.card) - 16;
  const double r = (c.v - c.min) / (c.max - c.min);
  lv_obj_set_width(c.fill, (int)(inner * fmax(0.04, fmin(1.0, r))));
  char buf[16];
  snprintf(buf, sizeof buf, c.fmt, c.v);
  set_text(c.value, buf);
}

void card_event(lv_event_t *e) {
  ValueCard &c = *(ValueCard *)lv_event_get_user_data(e);
  lv_point_t p;
  lv_indev_get_point(lv_indev_active(), &p);
  lv_area_t a;
  lv_obj_get_coords(c.card, &a);
  double r = (double)(p.x - a.x1 - 8) / (lv_area_get_width(&a) - 16);
  r = fmax(0, fmin(1, r));
  const double v = c.min + round(r * (c.max - c.min) / c.step) * c.step;
  const bool released = lv_event_get_code(e) == LV_EVENT_RELEASED;
  if (v != c.v || released) {
    c.v = v;
    card_render(c);
    if (c.changed) c.changed(v, released);
  }
}

ValueCard *value_card(lv_obj_t *parent, int x, int y, int w, int h, const char *name, const char *unit,
                      uint32_t accent, uint32_t fill, const lv_font_t *vfont, double min, double max,
                      double step, double v, const char *fmt, void (*changed)(double, bool)) {
  ValueCard &c = cards[n_cards++];
  c.card = box(parent, x, y, w, h, BG_MOD, R);
  lv_obj_set_style_border_color(c.card, hex(accent), 0);
  lv_obj_set_style_border_width(c.card, 2, 0);
  c.fill = box(c.card, 6, 6, 10, h - 12, fill, R - 6);
  lv_obj_t *n = text(c.card, name, &inter_20, hex(SUB));
  lv_obj_set_pos(n, 20, 14);
  c.unit = text(c.card, unit, &inter_20, hex(SUB));
  baseline_align(c.unit, LV_ALIGN_BOTTOM_RIGHT, -20, CARD_BASELINE);
  lv_obj_update_layout(c.unit);
  c.value = text(c.card, "", vfont, hex(accent));
  baseline_align(c.value, LV_ALIGN_BOTTOM_RIGHT, -(20 + lv_obj_get_width(c.unit) + 8), CARD_BASELINE);
  c.min = min; c.max = max; c.step = step; c.v = v; c.fmt = fmt; c.changed = changed;
  lv_obj_add_flag(c.card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(c.card, card_event, LV_EVENT_PRESSING, &c);
  lv_obj_add_event_cb(c.card, card_event, LV_EVENT_RELEASED, &c);
  lv_obj_update_layout(c.card);
  card_render(c);
  return &c;
}

lv_obj_t *round_button(lv_obj_t *parent, int x, int y, int d, const char *sym, const lv_font_t *f,
                       lv_obj_t **icon_out, lv_event_cb_t cb) {
  lv_obj_t *b = box(parent, x, y, d, d, 0x262626, LV_RADIUS_CIRCLE);
  lv_obj_t *i = text(b, sym, f, hex(0x9A9A9A));
  lv_obj_center(i);
  if (icon_out) *icon_out = i;
  on_click(b, cb);
  pressable(b);
  return b;
}

// Option button (pour pattern / scale unit): selected = tinted fill + accent text.
lv_obj_t *option_button(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *b = box(parent, x, y, w, h, SURFACE, R_SM);
  pressable(b);
  return b;
}

// Back control in the top-left: a 56x48 hit area around a chevron.
lv_obj_t *back_button(lv_obj_t *scr, const char *sym, lv_event_cb_t cb) {
  lv_obj_t *b = group(scr, M - 12, SB, 64, 52);
  lv_obj_t *chev = text(b, sym, &lv_font_montserrat_28, hex(TEXT));
  lv_obj_align(chev, LV_ALIGN_LEFT_MID, 12, 0);
  on_click(b, cb);
  pressable(b);
  return b;
}

// Module screen header: down-chevron (back) + the module's dot icon.
void module_header(lv_obj_t *scr, Dots kind, lv_event_cb_t back) {
  back_button(scr, LV_SYMBOL_DOWN, back);
  lv_obj_t *d = dot_icon(scr, kind, hex(TEXT), 6, 4);
  lv_obj_align(d, LV_ALIGN_TOP_MID, 0, SB + 4);
}

// The big readout row under the header (weight / timer).
lv_obj_t *readout(lv_obj_t *scr, const char *t, int x, bool right_aligned_to_x = false) {
  lv_obj_t *l = text(scr, t, &inter_34b, hex(TEXT));
  if (right_aligned_to_x) {
    lv_obj_set_width(l, 200);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    x -= 200;
  }
  lv_obj_set_pos(l, x, 96);
  return l;
}

// ============================================================================
// navigation
// ============================================================================
void build_detail();
void refresh_detail();

void go_home_from_detail(lv_event_t * = nullptr) { load(scr_home, Anim::PopRight); }
void go_home_from_module() { load(scr_home, Anim::DropDown); }

void set_tab(int t) {
  show(page_home, t == 0);
  show(page_recipes, t == 1);
  show(page_settings, t == 2);
  for (int i = 0; i < 3; ++i) set_text_color(tab_lbl[i], hex(i == t ? TEXT : MUTED), 0);
}

void open_recipe(uint32_t uid, Anim a = Anim::PushLeft) {
  if (!find(uid)) return;
  cur = uid;
  build_detail();
  load(scr_detail, a);
}

void on_recipe_tile(lv_event_t *e) {
  open_recipe((uint32_t)(uintptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e)));
}

// ============================================================================
// STATUS BAR (on the top layer, shared by every screen)
// ============================================================================
void build_status_bar() {
  lv_obj_t *bar = group(lv_layer_top(), 0, 0, W, SB);
  sb_clock = text(bar, "", &inter_16, hex(TEXT));
  lv_obj_align(sb_clock, LV_ALIGN_CENTER, 0, 2);  // left side holds the machine name on Home
  sb_bt = text(bar, LV_SYMBOL_BLUETOOTH, &lv_font_montserrat_16, hex(MUTED));
  lv_obj_align(sb_bt, LV_ALIGN_RIGHT_MID, -M, 2);
  sb_wifi = text(bar, LV_SYMBOL_WIFI, &lv_font_montserrat_16, hex(MUTED));
  lv_obj_align_to(sb_wifi, sb_bt, LV_ALIGN_OUT_LEFT_MID, -14, 0);
  show(sb_wifi, false);  // shown once a network is set up
}

// ============================================================================
// HOME
// ============================================================================
void open_grinder(lv_event_t *) {
  load(scr_grinder, Anim::RiseUp);
  post(BleCmdType::GrinderEnter, g_size, g_rpm);
}
void open_brewer(lv_event_t *) {
  load(scr_brewer, Anim::RiseUp);
  post(BleCmdType::BrewerEnter, xb::pattern_to_byte(b_pattern), 0, xb::temp_display_to_wire(b_temp));
}
void open_scale(lv_event_t *) {
  load(scr_scale, Anim::RiseUp);
  post(BleCmdType::ScaleEnter);
}

const int FAV_W = 160, FAV_H = 100;

void build_favorites() {
  lv_obj_clean(fav_row);
  int x = 0;
  for (size_t i = 0; i < lib.size(); ++i) {
    if (!lib[i].favorite) continue;
    lv_obj_t *t = recipe_tile(fav_row, x, 0, FAV_W, FAV_H, lib[i], true);
    on_click(t, on_recipe_tile);
    pressable(t);
    x += FAV_W + G;
  }
  if (x == 0) {
    lv_obj_t *e = text(fav_row, "No favorites yet", &inter_20, hex(0xBDB4AC));
    lv_obj_align(e, LV_ALIGN_LEFT_MID, 0, 0);
  }
}

void build_home_page(lv_obj_t *page) {
  // machine name + connection
  home_name = text(page, "XBLOOM", &inter_20b, hex(TEXT));
  lv_obj_set_pos(home_name, M, 6);  // shares the status-bar row
  home_chev = text(page, LV_SYMBOL_RIGHT, &lv_font_montserrat_16, hex(TEXT));
  home_spin = lv_spinner_create(page);
  lv_obj_set_size(home_spin, 16, 16);
  lv_obj_remove_flag(home_spin, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(home_spin, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_width(home_spin, 3, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(home_spin, hex(HAIRLINE), LV_PART_MAIN);
  lv_obj_set_style_arc_color(home_spin, hex(GREEN), LV_PART_INDICATOR);
  home_dot = box(page, 0, 0, 10, 10, GREEN, LV_RADIUS_CIRCLE);
  home_status = text(page, "Connecting", &inter_16, hex(MUTED));

  // module tiles (as tall as the favorites panel below allows)
  const int fav_h = 168, fav_y = H - TABBAR_H - G - fav_h;
  const int tile_y = SB + 8, tile_h = fav_y - G - tile_y, tile_w = (W - 2 * M - 2 * G) / 3;
  struct { const char *name; Dots d; uint32_t bg, fg; lv_event_cb_t cb; } tiles[3] = {
      {"Grinder", Dots::Grinder, GRINDER_BG, GRINDER_FG, open_grinder},
      {"Brewer", Dots::Brewer, BREWER_BG, BREWER_FG, open_brewer},
      {"Scale", Dots::Scale, SCALE_BG, SCALE_FG, open_scale},
  };
  for (int i = 0; i < 3; ++i) {
    lv_obj_t *t = box(page, M + i * (tile_w + G), tile_y, tile_w, tile_h, tiles[i].bg, R);
    lv_obj_t *d = dot_icon(t, tiles[i].d, hex(tiles[i].fg), 7, 5);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_t *l = text(t, tiles[i].name, &inter_26b, hex(tiles[i].fg));
    baseline_align(l, LV_ALIGN_BOTTOM_MID, 0, 32);
    on_click(t, tiles[i].cb);
    pressable(t);
  }

  // favorites
  lv_obj_t *fav = box(page, M, fav_y, W - 2 * M, fav_h, FAV_PANEL, R);
  lv_obj_t *ft = text(fav, "Favorite Recipes", &inter_20b, hex(TEXT));
  lv_obj_set_pos(ft, 20, 16);
  fav_row = lv_obj_create(fav);
  lv_obj_remove_style_all(fav_row);
  lv_obj_set_pos(fav_row, 20, fav_h - 20 - FAV_H);
  lv_obj_set_size(fav_row, W - 2 * M - 20, FAV_H);
  lv_obj_set_scroll_dir(fav_row, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(fav_row, LV_SCROLLBAR_MODE_OFF);
  build_favorites();
}

// ============================================================================
// MY RECIPES
// ============================================================================
const int ROW_H = 88, TILE = 72;

void build_recipe_list() {
  lv_obj_clean(rec_list);
  int y = 0;
  for (size_t i = 0; i < lib.size(); ++i) {
    if (seg_of(lib[i]) != seg) continue;
    lv_obj_t *row = group(rec_list, 0, y, W - 2 * M, ROW_H);
    lv_obj_set_user_data(row, (void *)(uintptr_t)lib[i].uid);
    on_click(row, on_recipe_tile);
    pressable(row);
    recipe_tile(row, 0, (ROW_H - TILE) / 2, TILE, TILE, lib[i], false);
    lv_obj_t *n = text(row, lib[i].r.name, &inter_20b, hex(TEXT));
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_obj_set_width(n, W - 2 * M - TILE - 80);
    lv_obj_set_pos(n, TILE + 20, 18);
    char buf[64];
    recipe_line(lib[i].r, buf, sizeof buf);
    lv_obj_t *s = text(row, buf, &inter_16, hex(MUTED));
    lv_obj_set_pos(s, TILE + 20, 48);
    lv_obj_t *chev = text(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_16, hex(0x48484A));
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -4, 0);
    if (lib[i].dirty && lib[i].kind != KIND_SEED && cloud::state() == cloud::Acct::SignedIn) {
      lv_obj_t *d = text(row, "Not saved to account", &inter_14, hex(0xE0A458));
      lv_obj_align(d, LV_ALIGN_RIGHT_MID, -28, 0);
    }
    y += ROW_H + 8;
  }
  if (y == 0) {
    lv_obj_t *e = text(rec_list, seg == 1 ? "Recipes saved from share links appear here." :
                                 seg == 0 ? "Sign in under Settings to bring in your recipes." : "",
                       &inter_20, hex(MUTED));
    lv_obj_set_pos(e, 4, 16);
  }
}

void set_seg(int s) {
  seg = s;
  const int third = (W - 2 * M - 8) / 3;
  lv_obj_set_x(seg_pill, 4 + s * third);
  for (int i = 0; i < 3; ++i) set_text_color(seg_lbl[i], hex(i == s ? 0x000000 : MUTED), 0);
  build_recipe_list();
}

void build_recipes_page(lv_obj_t *page) {
  lv_obj_t *t = text(page, "My Recipes", &inter_34b, hex(TEXT));
  lv_obj_set_pos(t, M, SB + 2);
  pill(page, W - M - 120, SB + 4, 120, 44, "+  New", 0x2C2C2E, TEXT, [](lv_event_t *) { open_editor(0); });
  const int seg_y = 94, seg_h = 48, half = (W - 2 * M - 8) / 3;
  lv_obj_t *segbox = box(page, M, seg_y, W - 2 * M, seg_h, SEG_BG, seg_h / 2);
  seg_pill = box(segbox, 4, 4, half, seg_h - 8, 0xFFFFFF, (seg_h - 8) / 2);
  const char *names[3] = {"Created", "Shared", "Starter"};
  for (int i = 0; i < 3; ++i) {
    lv_obj_t *hit = group(segbox, 4 + i * half, 0, half, seg_h);
    seg_lbl[i] = text(hit, names[i], &inter_20b, hex(MUTED));
    lv_obj_center(seg_lbl[i]);
    on_click(hit, [](lv_event_t *e) { set_seg((int)(intptr_t)lv_event_get_user_data(e)); }, (void *)(intptr_t)i);
  }
  const int list_y = seg_y + seg_h + G;
  rec_list = lv_obj_create(page);
  lv_obj_remove_style_all(rec_list);
  lv_obj_set_pos(rec_list, M, list_y);
  lv_obj_set_size(rec_list, W - 2 * M, H - TABBAR_H - list_y);
  lv_obj_set_scroll_dir(rec_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(rec_list, LV_SCROLLBAR_MODE_OFF);
  set_seg(0);
}

// ============================================================================
// SETTINGS (third tab) + Wi-Fi, password and software-update screens
// ============================================================================
lv_obj_t *scr_wifi, *scr_pass, *scr_update;
lv_obj_t *st_wifi_val, *st_ver_val, *st_machine_val, *sw_clock, *sw_night, *st_sync_val, *st_sync_btn, *st_sleep_dd, *st_ccolor_dd;
void refresh_screen_prefs();
void refresh_sync_row();
lv_obj_t *wf_list, *wf_status_title, *wf_status_sub, *wf_forget, *wf_scan_lbl;
lv_obj_t *pw_title, *pw_ta, *pw_show_lbl;
lv_obj_t *up_steps, *up_url, *up_ip, *up_bar, *up_status, *up_wifi_btn;
char pw_ssid[33];
bool wf_scanning_shown = false;
net::Wifi last_wifi = (net::Wifi)255;

// A titled group of rows (iOS-style inset list).
lv_obj_t *section(lv_obj_t *parent, const char *title, int x, int y, int w, int rows, int row_h) {
  lv_obj_t *t = text(parent, title, &inter_14, hex(MUTED));
  lv_obj_set_pos(t, x + 4, y);
  return box(parent, x, y + 24, w, rows * row_h, 0x1C1C1E, R_SM);
}

// One row: label on the left, value (+ chevron when tappable) on the right.
lv_obj_t *row(lv_obj_t *card, int i, int h, const char *label, lv_obj_t **value_out, lv_event_cb_t cb) {
  const int w = lv_obj_get_style_width(card, 0);
  lv_obj_t *r = group(card, 0, i * h, w, h);
  if (i > 0) box(r, 16, 0, w - 16, 1, HAIRLINE, 0);
  lv_obj_t *l = text(r, label, &inter_20, hex(TEXT));
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 16, 0);
  int right = -16;
  if (cb) {
    lv_obj_t *ch = text(r, LV_SYMBOL_RIGHT, &lv_font_montserrat_16, hex(0x48484A));
    lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -16, 0);
    right = -40;
    on_click(r, cb);
    lv_obj_set_style_bg_color(r, hex(0x2C2C2E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
  }
  if (value_out) {
    *value_out = text(r, "", &inter_20, hex(MUTED));
    lv_label_set_long_mode(*value_out, LV_LABEL_LONG_DOT);
    lv_obj_update_layout(l);
    lv_obj_set_width(*value_out, w - 16 - lv_obj_get_width(l) - 24 + right);  // all the space the label leaves
    lv_obj_set_style_text_align(*value_out, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(*value_out, LV_ALIGN_RIGHT_MID, right, 0);
  }
  return r;
}

lv_obj_t *toggle(lv_obj_t *row_obj, bool on, lv_event_cb_t cb) {
  lv_obj_t *s = lv_switch_create(row_obj);
  lv_obj_set_size(s, 56, 32);
  lv_obj_align(s, LV_ALIGN_RIGHT_MID, -16, 0);
  lv_obj_set_style_bg_color(s, hex(0x39393D), LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, hex(GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(s, hex(0xFFFFFF), LV_PART_KNOB);
  lv_obj_set_style_pad_all(s, -3, LV_PART_KNOB);
  if (on) lv_obj_add_state(s, LV_STATE_CHECKED);
  lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, nullptr);
  return s;
}

// A compact dark drop-down list on the right of a settings row.
lv_obj_t *dropdown(lv_obj_t *row_obj, const char *options, lv_event_cb_t cb) {
  lv_obj_t *d = lv_dropdown_create(row_obj);
  lv_dropdown_set_options_static(d, options);
  lv_obj_set_size(d, 140, 38);
  lv_obj_align(d, LV_ALIGN_RIGHT_MID, -12, 0);
  lv_obj_set_style_bg_color(d, hex(0x2C2C2E), 0);
  lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(d, 0, 0);
  lv_obj_set_style_radius(d, 10, 0);
  lv_obj_set_style_text_color(d, hex(TEXT), 0);
  lv_obj_set_style_text_font(d, &inter_16, 0);
  lv_obj_set_style_pad_hor(d, 12, 0);
  lv_obj_set_style_pad_ver(d, 9, 0);
  lv_obj_set_style_text_font(d, &lv_font_montserrat_14, LV_PART_INDICATOR);  // the arrow is an icon glyph
  lv_obj_set_style_text_color(d, hex(MUTED), LV_PART_INDICATOR);
  lv_obj_t *l = lv_dropdown_get_list(d);
  lv_obj_set_style_bg_color(l, hex(0x2C2C2E), 0);
  lv_obj_set_style_border_width(l, 0, 0);
  lv_obj_set_style_radius(l, 12, 0);
  lv_obj_set_style_text_color(l, hex(TEXT), 0);
  lv_obj_set_style_text_font(l, &inter_20, 0);
  lv_obj_set_style_text_line_space(l, 18, 0);
  lv_obj_set_style_pad_all(l, 10, 0);
  lv_obj_set_style_max_height(l, 300, 0);
  lv_obj_set_style_bg_color(l, hex(0x48484A), LV_PART_SELECTED | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(l, hex(0x3A3A3C), LV_PART_SELECTED | LV_STATE_PRESSED);
  lv_obj_set_style_text_color(l, hex(TEXT), LV_PART_SELECTED | LV_STATE_CHECKED);
  lv_obj_add_event_cb(d, cb, LV_EVENT_VALUE_CHANGED, nullptr);
  return d;
}

lv_obj_t *pill(lv_obj_t *parent, int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg,
               lv_event_cb_t cb) {
  lv_obj_t *b = box(parent, x, y, w, h, bg, h / 2);
  lv_obj_t *l = text(b, label, &inter_20b, hex(fg));
  lv_obj_center(l);
  on_click(b, cb);
  pressable(b);
  return b;
}

// Header for sub-screens: back chevron + title on one row.
void sub_header(lv_obj_t *scr, const char *title, lv_event_cb_t back, lv_obj_t **title_out = nullptr) {
  back_button(scr, LV_SYMBOL_LEFT, back);
  lv_obj_t *t = text(scr, title, &inter_26b, hex(TEXT));
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  lv_obj_set_width(t, W - 2 * M - 120);
  lv_obj_set_pos(t, M + 44, SB + 10);
  if (title_out) *title_out = t;
}

void open_wifi(lv_event_t * = nullptr);
void open_update(lv_event_t * = nullptr);
void back_to_settings(lv_event_t *) { load(scr_home, Anim::PopRight); }

void build_settings_page(lv_obj_t *page) {
  lv_obj_t *t = text(page, "Settings", &inter_34b, hex(TEXT));
  lv_obj_set_pos(t, M, SB + 2);
  const int rh = 52, cw = (W - 2 * M - G) / 2, y0 = 84, x2 = M + cw + G;

  lv_obj_t *c1 = section(page, "CONNECTION", M, y0, cw, 4, rh);
  row(c1, 0, rh, "Wi-Fi", &st_wifi_val, open_wifi);
  row(c1, 1, rh, "Software update", &st_ver_val, open_update);
  row(c1, 2, rh, "xBloom account", &st_acc_val, open_account);
  // Sync: a button on the right, the last sync time next to it
  lv_obj_t *rs = row(c1, 3, rh, "Recipes", nullptr, nullptr);
  st_sync_btn = pill(rs, cw - 16 - 120, (rh - 36) / 2, 120, 36, "Sync now", 0xFFFFFF, 0x000000, [](lv_event_t *) {
    if (cloud::state() == cloud::Acct::SignedIn) { cloud::sync_now(); refresh_sync_row(); }
    else open_account();
  });
  st_sync_val = text(rs, "", &inter_16, hex(MUTED));
  lv_obj_align(st_sync_val, LV_ALIGN_RIGHT_MID, -(16 + 120 + 12), 0);
  set_text(st_ver_val, FW_VERSION);

  lv_obj_t *c2 = section(page, "SCREEN", x2, y0, cw, 4, rh);
  lv_obj_t *r1 = row(c2, 0, rh, "Clock when idle", nullptr, nullptr);
  sw_clock = toggle(r1, net::pref_clock(), [](lv_event_t *e) {
    net::set_pref_clock(lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED));
  });
  lv_obj_t *rsl = row(c2, 1, rh, "Sleep after", nullptr, nullptr);
  st_sleep_dd = dropdown(rsl, "1 min\n3 min\n5 min\n10 min\nNever", [](lv_event_t *e) {
    static const int opts[] = {1, 3, 5, 10, 0};
    net::set_pref_sleep_min(opts[lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e)) % 5]);
  });
  lv_obj_t *rcc = row(c2, 2, rh, "Clock colour", nullptr, nullptr);
  st_ccolor_dd = dropdown(rcc, "White\nBlue\nBrown", [](lv_event_t *e) {
    net::set_pref_clock_color(lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e)) % 3);
  });
  lv_obj_t *r2 = row(c2, 3, rh, "Off at night (11 PM-6 AM)", nullptr, nullptr);
  sw_night = toggle(r2, net::pref_night_off(), [](lv_event_t *e) {
    net::set_pref_night_off(lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED));
  });
  refresh_screen_prefs();

  lv_obj_t *c3 = section(page, "ABOUT", M, y0 + 24 + 4 * rh + G, cw, 1, rh);
  row(c3, 0, rh, "Machine", &st_machine_val, nullptr);
  set_text(st_machine_val, "Not connected");
}

void format_clock(const struct tm &t, char *hm, size_t n);
void refresh_screen_prefs() {
  static const int opts[] = {1, 3, 5, 10, 0};
  const int m = net::pref_sleep_min();
  for (int i = 0; i < 5; ++i)
    if (opts[i] == m) lv_dropdown_set_selected(st_sleep_dd, i);
  lv_dropdown_set_selected(st_ccolor_dd, net::pref_clock_color() % 3);
}
void refresh_sync_row() {
  if (!st_sync_val) return;
  if (cloud::state() != cloud::Acct::SignedIn) { set_text(st_sync_val, "Not signed in"); return; }
  if (cloud::busy()) { set_text(st_sync_val, "Syncing..."); return; }
  const time_t ls = cloud::last_sync();
  if (!ls) { set_text(st_sync_val, ""); return; }
  struct tm t;
  localtime_r(&ls, &t);
  char hm[12], b[24];
  format_clock(t, hm, sizeof hm);
  snprintf(b, sizeof b, "Synced %s", hm);
  set_text(st_sync_val, b);
}

// ---- Wi-Fi list ----------------------------------------------------------
void open_password(const char *ssid);

void on_network(lv_event_t *e) {
  const net::Ap *ap = (const net::Ap *)lv_event_get_user_data(e);
  if (ap->secure) open_password(ap->ssid);
  else net::join(ap->ssid, "");
}

net::Ap wf_aps[24];
int wf_n = 0;

void build_network_list() {
  lv_obj_clean(wf_list);
  wf_n = net::scan_results(wf_aps, 24);
  const int rh = 56, w = lv_obj_get_style_width(wf_list, 0);
  if (wf_n == 0) {
    lv_obj_t *e = text(wf_list, net::scan_running() ? "Looking for networks..." : "No networks found",
                       &inter_20, hex(MUTED));
    lv_obj_set_pos(e, 16, 16);
    lv_obj_set_height(wf_list, 56);
    return;
  }
  const bool joined = net::state() == net::Wifi::Connected;
  for (int i = 0; i < wf_n; ++i) {
    lv_obj_t *r = group(wf_list, 0, i * rh, w, rh);
    if (i > 0) box(r, 52, 0, w - 52, 1, HAIRLINE, 0);
    const int8_t q = wf_aps[i].rssi;
    lv_obj_t *ic = text(r, LV_SYMBOL_WIFI, &lv_font_montserrat_20, hex(q > -60 ? TEXT : q > -75 ? SUB : MUTED));
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_t *n = text(r, wf_aps[i].ssid, &inter_20, hex(TEXT));
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_obj_set_width(n, w - 150);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 52, 0);
    const bool here = joined && strcmp(wf_aps[i].ssid, net::ssid()) == 0;
    lv_obj_t *tag = text(r, here ? LV_SYMBOL_OK : wf_aps[i].secure ? "Secured" : "Open",
                         here ? &lv_font_montserrat_20 : &inter_16, hex(here ? GREEN : MUTED));
    lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -16, 0);
    on_click(r, on_network, &wf_aps[i]);
    lv_obj_set_style_bg_color(r, hex(0x2C2C2E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
  }
  lv_obj_set_height(wf_list, LV_MIN(wf_n * rh, H - M - 96));
}

void refresh_wifi_status() {
  const char *title = "Not connected", *sub = "Choose your network from the list.";
  char buf[96];
  switch (net::state()) {
    case net::Wifi::Connected:
      title = net::ssid();
      snprintf(buf, sizeof buf, "Connected\n%s", net::ip());
      sub = buf;
      break;
    case net::Wifi::Connecting:
      title = net::ssid();
      sub = "Connecting...";
      break;
    case net::Wifi::WrongPassword:
      title = net::ssid();
      sub = "Wrong password. Tap the network to try again.";
      break;
    case net::Wifi::NotFound:
      title = net::ssid();
      sub = "Network not found. Is it in range?";
      break;
    default: break;
  }
  set_text(wf_status_title, title);
  set_text(wf_status_sub, sub);
  show(wf_forget, net::has_saved_network());
}

void build_wifi() {
  scr_wifi = new_screen(BG);
  sub_header(scr_wifi, "Wi-Fi", back_to_settings);
  const int lw = 480, rx = M + lw + 20, rw = W - M - rx;
  wf_list = box(scr_wifi, M, 96, lw, 56, 0x1C1C1E, R_SM);
  lv_obj_add_flag(wf_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_scroll_dir(wf_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(wf_list, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *card = box(scr_wifi, rx, 96, rw, 200, 0x1C1C1E, R_SM);
  wf_status_title = text(card, "", &inter_20b, hex(TEXT));
  lv_label_set_long_mode(wf_status_title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(wf_status_title, rw - 32);
  lv_obj_set_pos(wf_status_title, 16, 16);
  wf_status_sub = text(card, "", &inter_16, hex(SUB));
  lv_label_set_long_mode(wf_status_sub, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(wf_status_sub, rw - 32);
  lv_obj_set_style_text_line_space(wf_status_sub, 4, 0);
  lv_obj_set_pos(wf_status_sub, 16, 48);
  wf_forget = pill(card, 16, 200 - 16 - 48, rw - 32, 48, "Forget network", 0x2C2C2E, 0xFF6B6B,
                   [](lv_event_t *) { net::forget(); });
  lv_obj_t *scan = pill(scr_wifi, rx, 96 + 200 + G, rw, 52, "Scan again", 0x2C2C2E, TEXT, [](lv_event_t *) {
    net::scan_start();
    wf_scanning_shown = true;
    build_network_list();
  });
  wf_scan_lbl = lv_obj_get_child(scan, 0);
}

void open_wifi(lv_event_t *) {
  net::scan_start();
  wf_scanning_shown = true;
  build_network_list();
  refresh_wifi_status();
  load(scr_wifi, Anim::PushLeft);
}

// ---- text entry (keyboard screen): Wi-Fi password, recipe name, account sign-in ----
typedef void (*TextDone)(const char *text);
TextDone te_done = nullptr;
lv_obj_t *te_back_to = nullptr, *te_show_btn, *te_action_lbl;

void te_submit(lv_event_t *) {
  char buf[96];
  strlcpy(buf, lv_textarea_get_text(pw_ta), sizeof buf);
  if (te_done) te_done(buf);
}

void build_text_entry() {
  scr_pass = new_screen(BG);
  sub_header(scr_pass, "", [](lv_event_t *) { load(te_back_to, Anim::PopRight); }, &pw_title);
  pw_ta = lv_textarea_create(scr_pass);
  lv_textarea_set_one_line(pw_ta, true);
  lv_obj_set_pos(pw_ta, M, 100);
  lv_obj_set_style_text_font(pw_ta, &inter_20, 0);
  set_text_color(pw_ta, hex(TEXT), 0);
  lv_obj_set_style_bg_color(pw_ta, hex(0x1C1C1E), 0);
  lv_obj_set_style_border_width(pw_ta, 0, 0);
  lv_obj_set_style_radius(pw_ta, R_SM, 0);
  lv_obj_set_style_pad_hor(pw_ta, 16, 0);
  lv_obj_set_style_pad_ver(pw_ta, 14, 0);
  set_text_color(pw_ta, hex(MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_color(pw_ta, hex(TEXT), LV_PART_CURSOR);
  lv_obj_add_state(pw_ta, LV_STATE_FOCUSED);

  te_show_btn = pill(scr_pass, W - M - 2 * 120 - 12, 100, 120, 56, "Show", 0x2C2C2E, TEXT, [](lv_event_t *) {
    const bool hidden = lv_textarea_get_password_mode(pw_ta);
    lv_textarea_set_password_mode(pw_ta, !hidden);
    set_text(pw_show_lbl, hidden ? "Hide" : "Show");
  });
  pw_show_lbl = lv_obj_get_child(te_show_btn, 0);
  lv_obj_t *act = pill(scr_pass, W - M - 120, 100, 120, 56, "Join", 0xFFFFFF, 0x000000, te_submit);
  te_action_lbl = lv_obj_get_child(act, 0);

  lv_obj_t *kb = lv_keyboard_create(scr_pass);
  lv_obj_set_size(kb, W, H - 176);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(kb, pw_ta);
  lv_obj_set_style_bg_color(kb, hex(0x1C1C1E), 0);
  lv_obj_set_style_border_width(kb, 0, 0);
  lv_obj_set_style_pad_all(kb, 8, 0);
  lv_obj_set_style_pad_gap(kb, 8, 0);
  lv_obj_set_style_bg_color(kb, hex(0x3A3A3C), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(kb, hex(0x5A5A5E), LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(kb, hex(0x2A2A2C), LV_PART_ITEMS | LV_STATE_CHECKED);
  set_text_color(kb, hex(TEXT), LV_PART_ITEMS);
  set_text_color(kb, hex(TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_opa(kb, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, LV_PART_ITEMS);
  lv_obj_set_style_radius(kb, 8, LV_PART_ITEMS);
  lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
  lv_obj_add_event_cb(kb, te_submit, LV_EVENT_READY, nullptr);  // the keyboard's check key
  lv_obj_add_event_cb(kb, [](lv_event_t *) { load(te_back_to, Anim::PopRight); }, LV_EVENT_CANCEL, nullptr);
}

void open_text_entry(const char *title, const char *initial, const char *placeholder, bool secret,
                     const char *action, lv_obj_t *back_to, TextDone done, int max_len = 63) {
  set_text(pw_title, title);
  lv_textarea_set_max_length(pw_ta, max_len);
  lv_textarea_set_text(pw_ta, initial);
  lv_textarea_set_placeholder_text(pw_ta, placeholder);
  lv_textarea_set_password_mode(pw_ta, secret);
  set_text(pw_show_lbl, "Show");
  set_text(te_action_lbl, action);
  show(te_show_btn, secret);
  lv_obj_set_size(pw_ta, W - 2 * M - (secret ? 2 : 1) * (120 + 12), 56);
  te_back_to = back_to;
  te_done = done;
  load(scr_pass, Anim::PushLeft);
}

void open_password(const char *ssid) {
  strlcpy(pw_ssid, ssid, sizeof pw_ssid);
  char buf[64];
  snprintf(buf, sizeof buf, "Join \"%s\"", ssid);
  open_text_entry(buf, "", "Password", true, "Join", scr_wifi, [](const char *t) {
    net::join(pw_ssid, t);
    refresh_wifi_status();
    load(scr_wifi, Anim::PopRight);
  });
}

// ---- software update ------------------------------------------------------
void refresh_update() {
  const bool online = net::state() == net::Wifi::Connected;
  show(up_wifi_btn, !online);
  show(up_url, online);
  show(up_ip, online);
  char buf[80];
  if (!online) {
    set_text(up_steps, "Connect the panel to Wi-Fi first.");
  } else {
    set_text(up_steps, "On your phone or computer (same Wi-Fi), open:");
    snprintf(buf, sizeof buf, "http://%s", net::hostname());
    set_text(up_url, buf);
    snprintf(buf, sizeof buf, "or  http://%s     then choose the update file.", net::ip());
    set_text(up_ip, buf);
  }
  const net::Upd u = net::update_state();
  lv_bar_set_value(up_bar, u == net::Upd::Idle ? 0 : net::update_progress(), LV_ANIM_OFF);
  show(up_bar, u != net::Upd::Idle);
  switch (u) {
    case net::Upd::Idle: set_text(up_status, online ? "Waiting for the file..." : ""); break;
    case net::Upd::Receiving:
      snprintf(buf, sizeof buf, "Installing... %d%%   Keep the panel powered.", net::update_progress());
      set_text(up_status, buf);
      break;
    case net::Upd::Done: set_text(up_status, "Installed. Restarting..."); break;
    case net::Upd::Failed:
      snprintf(buf, sizeof buf, "Failed: %s", net::update_error());
      set_text(up_status, buf);
      break;
  }
  set_text_color(up_status, hex(u == net::Upd::Failed ? 0xFF6B6B : u == net::Upd::Done ? GREEN : SUB),
                              0);
}

void build_update() {
  scr_update = new_screen(BG);
  sub_header(scr_update, "Software update", [](lv_event_t *) {
    if (net::update_state() == net::Upd::Receiving) return;  // stay while a file is installing
    net::update_page(false);
    load(scr_home, Anim::PopRight);
  });
  lv_obj_t *card = box(scr_update, M, 96, W - 2 * M, H - M - 96, 0x1C1C1E, R_SM);
  char buf[48];
  snprintf(buf, sizeof buf, "Installed version %s", FW_VERSION);
  lv_obj_t *v = text(card, buf, &inter_20b, hex(TEXT));
  lv_obj_set_pos(v, 24, 22);
  up_steps = text(card, "", &inter_20, hex(SUB));
  lv_obj_set_pos(up_steps, 24, 72);
  up_url = text(card, "", &inter_34b, hex(TEXT));
  lv_obj_set_pos(up_url, 24, 106);
  up_ip = text(card, "", &inter_20, hex(SUB));
  lv_obj_set_pos(up_ip, 24, 160);
  up_wifi_btn = pill(card, 24, 110, 240, 52, "Wi-Fi settings", 0xFFFFFF, 0x000000, [](lv_event_t *) {
    net::update_page(false);
    open_wifi();
  });
  up_bar = lv_bar_create(card);
  lv_obj_set_size(up_bar, W - 2 * M - 48, 10);
  lv_obj_set_pos(up_bar, 24, 230);
  lv_obj_set_style_bg_color(up_bar, hex(0x2C2C2E), 0);
  lv_obj_set_style_bg_color(up_bar, hex(GREEN), LV_PART_INDICATOR);
  lv_bar_set_range(up_bar, 0, 100);
  up_status = text(card, "", &inter_20, hex(SUB));
  lv_obj_set_pos(up_status, 24, 252);
  lv_obj_t *note = text(card, "If an update ever fails to start, the panel goes back to this version by itself.",
                        &inter_16, hex(MUTED));
  lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, 24, -20);
}

void open_update(lv_event_t *) {
  net::update_page(true);
  refresh_update();
  load(scr_update, Anim::PushLeft);
}

// ---- clock (shown instead of a black screen) ------------------------------
lv_obj_t *clock_layer;
uint32_t clock_color = dotfx::WHITE;
uint32_t clock_color_pref() {
  const int c = net::pref_clock_color();
  return c == 1 ? dotfx::BLUE : c == 2 ? 0xC8925A : dotfx::WHITE;
}

void build_clock() {
  clock_layer = dotfx::clock_create(lv_layer_top());
  show(clock_layer, false);
}

void format_clock(const struct tm &t, char *hm, size_t n) {
  int h = t.tm_hour % 12;
  snprintf(hm, n, "%d:%02d", h ? h : 12, t.tm_min);
}

void refresh_clock() {
  struct tm t;
  if (!net::local_time(&t)) return;
  char hm[16], date[32];
  format_clock(t, hm, sizeof hm);
  char day[8], mon[8];
  strftime(day, sizeof day, "%a", &t);
  strftime(mon, sizeof mon, "%b", &t);
  snprintf(date, sizeof date, "%s %d %s", day, t.tm_mday, mon);
  dotfx::clock_set(clock_layer, hm, date, clock_color_pref());
}

// ---- status-bar + settings values that follow the network -----------------
void refresh_net() {
  const net::Wifi s = net::state();
  show(sb_wifi, s != net::Wifi::Off);
  set_text_color(sb_wifi, hex(s == net::Wifi::Connected ? TEXT : MUTED), 0);
  struct tm t;
  if (net::local_time(&t)) {
    char hm[12];
    format_clock(t, hm, sizeof hm);
    set_text(sb_clock, hm);
  }
  if (s != last_wifi) {
    last_wifi = s;
    set_text(st_wifi_val, s == net::Wifi::Connected ? net::ssid()
                                   : s == net::Wifi::Off     ? "Off"
                                   : s == net::Wifi::Connecting ? "Connecting..."
                                                             : "Not connected");
    if (s == net::Wifi::WrongPassword) show_toast("Wrong Wi-Fi password");
    if (lv_screen_active() == scr_wifi) build_network_list();
  }
  if (lv_screen_active() == scr_wifi) {
    refresh_wifi_status();
    if (wf_scanning_shown && !net::scan_running()) {
      wf_scanning_shown = false;
      build_network_list();
    }
    set_text(wf_scan_lbl, net::scan_running() ? "Scanning..." : "Scan again");
  }
  if (lv_screen_active() == scr_update) refresh_update();
  if (!lv_obj_has_flag(clock_layer, LV_OBJ_FLAG_HIDDEN)) refresh_clock();
}

void build_home_screen() {
  scr_home = new_screen(BG);
  page_home = box(scr_home, 0, 0, W, H - TABBAR_H, BG, 0);
  page_recipes = box(scr_home, 0, 0, W, H - TABBAR_H, BG, 0);
  page_settings = box(scr_home, 0, 0, W, H - TABBAR_H, BG, 0);
  build_home_page(page_home);
  build_recipes_page(page_recipes);
  build_settings_page(page_settings);
  lv_obj_t *bar = box(scr_home, 0, H - TABBAR_H, W, TABBAR_H, TABBAR, 0);
  box(bar, 0, 0, W, 1, HAIRLINE, 0);
  const char *tabs[3] = {"xBloom", "Recipes", "Settings"};
  for (int i = 0; i < 3; ++i) {
    lv_obj_t *hit = group(bar, i * W / 3, 0, W / 3, TABBAR_H);
    tab_lbl[i] = text(hit, tabs[i], &inter_20b, hex(MUTED));
    lv_obj_center(tab_lbl[i]);
    on_click(hit, [](lv_event_t *e) { set_tab((int)(intptr_t)lv_event_get_user_data(e)); }, (void *)(intptr_t)i);
  }
  set_tab(0);
}

// ============================================================================
// RECIPE DETAIL
// ============================================================================
void on_start(lv_event_t *);
void on_stop(lv_event_t *);

void close_modal() { show(modal, false); }

void build_modal() {
  modal = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(modal);
  lv_obj_set_size(modal, W, H);
  lv_obj_set_style_bg_color(modal, hex(0x000000), 0);
  lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
  lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);
  const int dw = 456, dh = 240, pad = 28, bw = (dw - 2 * pad - G) / 2, bh = 56;
  lv_obj_t *dlg = box(modal, 0, 0, dw, dh, 0x1C1C1E, 24);
  lv_obj_center(dlg);
  lv_obj_t *t = text(dlg, "Place your cup", &inter_26b, hex(TEXT));
  lv_obj_set_pos(t, pad, pad);
  lv_obj_t *s = text(dlg, "The recipe is loaded on the machine.\nPut your cup on the scale, then start.",
                     &inter_16, hex(SUB));
  lv_obj_set_style_text_line_space(s, 4, 0);
  lv_obj_set_pos(s, pad, pad + 46);
  lv_obj_t *cancel = box(dlg, pad, dh - pad - bh, bw, bh, 0x2C2C2E, bh / 2);
  lv_obj_center(text(cancel, "Cancel", &inter_20b, hex(TEXT)));
  pressable(cancel);
  on_click(cancel, [](lv_event_t *) {
    close_modal();
    post(BleCmdType::QuitRecipe);
    flow = Flow::Idle;
    active = 0;
    refresh_detail();
  });
  lv_obj_t *go = box(dlg, pad + bw + G, dh - pad - bh, bw, bh, 0xFFFFFF, bh / 2);
  lv_obj_center(text(go, "Brew", &inter_20b, hex(0x000000)));
  pressable(go);
  on_click(go, [](lv_event_t *) {
    close_modal();
    post(BleCmdType::Execute);  // the only path that starts a recipe brew
    flow = Flow::Starting;
    refresh_detail();
  });
  show(modal, false);
}

// Detail layout: two columns under the back row.
const int D_TOP = 92, D_LEFT_W = 360, D_RIGHT_X = M + D_LEFT_W + 24, D_RIGHT_W = W - M - D_RIGHT_X;
const int D_HDR_H = 160, D_STATS_Y = D_TOP + D_HDR_H + 12, D_STATS_H = 84;
const int D_BTN_H = 96, D_BTN_Y = H - M - D_BTN_H;

void build_detail() {
  const PanelRecipe *pp = find(cur);
  if (!pp) return;
  const PanelRecipe &p = *pp;
  const xb::Recipe &r = p.r;
  const uint32_t bg = p.color;
  const lv_color_t ink = darker(bg, 70), dark = hex(0x2A2A2A), ghost = lv_color_darken(hex(bg), 30);

  // Reuse the screen object (never delete a screen that may still be animating out).
  if (!scr_detail) scr_detail = new_screen(BG_MOD);
  else lv_obj_clean(scr_detail);
  lv_obj_set_style_bg_color(scr_detail, lv_color_mix(hex(bg), hex(BG_MOD), 40), 0);
  lv_obj_set_style_bg_grad_color(scr_detail, hex(BG_MOD), 0);
  lv_obj_set_style_bg_grad_dir(scr_detail, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_main_stop(scr_detail, 0, 0);
  lv_obj_set_style_bg_grad_stop(scr_detail, 150, 0);

  back_button(scr_detail, LV_SYMBOL_LEFT, go_home_from_detail);
  lv_obj_t *eb = group(scr_detail, W - M - 52, SB, 52, 52);
  lv_obj_center(text(eb, LV_SYMBOL_EDIT, &lv_font_montserrat_20, hex(TEXT)));
  on_click(eb, [](lv_event_t *) { open_editor(cur); });
  pressable(eb);
  lv_obj_t *hb = group(scr_detail, W - M - 52 - 8 - 52, SB, 52, 52);
  lv_obj_t *heart = heart_icon(hb, p.favorite ? 0xFF453A : 0x5A5A5E);
  lv_obj_center(heart);
  on_click(hb, [](lv_event_t *e) {
    toggle_favorite(cur);
    const PanelRecipe *q = find(cur);
    lv_obj_t *h = lv_obj_get_child((lv_obj_t *)lv_event_get_current_target(e), 0);
    heart_color(h, q && q->favorite ? 0xFF453A : 0x5A5A5E);
  });
  pressable(hb);

  // ---- header card ----
  lv_obj_t *hdr = box(scr_detail, M, D_TOP, D_LEFT_W, D_HDR_H, bg, R_SM);
  char n[4];
  snprintf(n, sizeof n, "%d", r.n_pours);
  lv_obj_t *num = text(hdr, n, &inter_120_bold, ghost);
  baseline_align(num, LV_ALIGN_BOTTOM_RIGHT, -16, -6);
  lv_obj_t *pw = text(hdr, "POURS", &inter_26b, ghost);
  lv_obj_align(pw, LV_ALIGN_TOP_RIGHT, -16, 12);
  lv_obj_t *cup = cup_icon(hdr, r.cup_type, ink);
  lv_obj_set_pos(cup, 20, 20);
  lv_obj_t *cl = text(hdr, recipes::cup_name(r.cup_type), &inter_20, ink);
  lv_obj_set_pos(cl, 52, 17);
  lv_obj_t *name = text(hdr, r.name, &inter_20, dark);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  lv_obj_set_width(name, D_LEFT_W - 140);
  lv_obj_set_pos(name, 20, 66);
  char buf[48];
  snprintf(buf, sizeof buf, "1:%g", ratio_of(r));
  lv_obj_t *ratio = text(hdr, buf, &inter_34b, dark);
  baseline_align(ratio, LV_ALIGN_BOTTOM_LEFT, 20, 22);
  lv_obj_t *sep = box(hdr, 0, 0, 2, 30, 0, 0);
  lv_obj_set_style_bg_color(sep, dark, 0);
  lv_obj_set_style_bg_opa(sep, LV_OPA_40, 0);
  lv_obj_update_layout(ratio);
  lv_obj_align_to(sep, ratio, LV_ALIGN_OUT_RIGHT_MID, 12, 2);
  snprintf(buf, sizeof buf, "%.0fml", r.total_water());
  lv_obj_t *vol = text(hdr, buf, &inter_34b, dark);
  lv_obj_update_layout(sep);
  baseline_align(vol, LV_ALIGN_BOTTOM_LEFT, lv_obj_get_x(sep) + 14, 22);

  // ---- stats (Dose / Grind / RPM; live readings while brewing) ----
  lv_obj_t *st = box(scr_detail, M, D_STATS_Y, D_LEFT_W, D_STATS_H, 0x2C2C2E, R_SM);
  lv_obj_set_style_border_color(st, hex(0x3A3A3C), 0);
  lv_obj_set_style_border_width(st, 1, 0);
  for (int i = 0; i < 3; ++i) {
    lv_obj_t *col = group(st, i * D_LEFT_W / 3, 0, D_LEFT_W / 3, D_STATS_H);
    d_stats[i][0] = text(col, "", &inter_16, hex(MUTED));
    lv_obj_align(d_stats[i][0], LV_ALIGN_TOP_MID, 0, 12);
    d_stats[i][1] = text(col, "", &inter_26b, lv_color_lighten(hex(bg), 40));
    d_stat_unit[i] = text(col, "", &inter_16, hex(SUB));
  }

  // ---- START pill + stop circle ----
  d_start = box(scr_detail, M, D_BTN_Y, D_LEFT_W, D_BTN_H, bg, D_BTN_H / 2);
  d_start_icon = text(d_start, LV_SYMBOL_PLAY, &lv_font_montserrat_28, hex(BG_MOD));
  lv_obj_align(d_start_icon, LV_ALIGN_TOP_MID, 0, 16);
  d_start_lbl = text(d_start, "START", &inter_20b, hex(BG_MOD));
  baseline_align(d_start_lbl, LV_ALIGN_BOTTOM_MID, 0, 18);
  on_click(d_start, on_start);
  pressable(d_start);
  d_stop = box(scr_detail, M + D_LEFT_W - D_BTN_H, D_BTN_Y, D_BTN_H, D_BTN_H, BG_MOD, LV_RADIUS_CIRCLE);
  lv_obj_set_style_border_color(d_stop, hex(bg), 0);
  lv_obj_set_style_border_width(d_stop, 4, 0);
  lv_obj_center(text(d_stop, LV_SYMBOL_STOP, &lv_font_montserrat_28, hex(bg)));
  on_click(d_stop, on_stop);
  pressable(d_stop);

  // ---- pours chart ----
  lv_obj_t *pt = text(scr_detail, "Pours", &inter_26b, hex(TEXT));
  lv_obj_set_pos(pt, D_RIGHT_X, D_TOP - 4);
  const int ch_y = D_TOP + 44, ch_h = H - M - ch_y;
  lv_obj_t *chart = group(scr_detail, D_RIGHT_X, ch_y, D_RIGHT_W, ch_h);
  double maxv = 1;
  for (int i = 0; i < r.n_pours; ++i) maxv = fmax(maxv, r.pours[i].volume_ml);
  d_ncols = r.n_pours;
  const int gap = 6, cw = (D_RIGHT_W - gap * (r.n_pours - 1)) / r.n_pours;
  const int base = ch_h - 104, bar_max = base - 40;  // room for the value label above
  const bool narrow = cw < 70;
  double cum = 0;
  for (int i = 0; i < r.n_pours; ++i) {
    const xb::Pour &po = r.pours[i];
    const int x = i * (cw + gap);
    if (i < r.n_pours - 1) box(chart, x + cw + gap / 2 - 1, 0, 1, ch_h, 0x262626, 0);
    const int h = (int)fmax(4, bar_max * po.volume_ml / maxv);
    lv_obj_t *bar = box(chart, x, base - h, cw, h, 0x3A3A3A, 4);
    lv_obj_t *fill = box(bar, 0, h, cw, 0, bg, 0);
    d_cols[i] = {bar, fill, po.volume_ml, cum};
    cum += po.volume_ml;

    auto centered = [&](const char *s, const lv_font_t *f, lv_color_t c, int y) {
      lv_obj_t *l = text(chart, s, f, c);
      lv_obj_set_width(l, cw);
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(l, x, y);
      return l;
    };
    snprintf(buf, sizeof buf, "%.0fml", po.volume_ml);
    centered(buf, narrow ? &inter_16 : &inter_20b, hex(TEXT), base - h - 30);

    lv_obj_t *row = group(chart, x, base + 14, cw, 22);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    pattern_icon(row, po.pattern, 18, hex(0xDDDDDD));
    snprintf(buf, sizeof buf, "%.0f°C", po.temp_c);
    text(row, buf, &inter_16, hex(SUB));

    char nm[24];
    if (narrow) {
      if (i == 0) strcpy(nm, "Bloom");
      else snprintf(nm, sizeof nm, "P%d", i + 1);
    } else {
      char tmp[24];
      strlcpy(nm, library::pour_name(p, i, tmp), sizeof nm);
    }
    centered(nm, narrow ? &inter_16 : &inter_20, hex(TEXT), base + 46);
    snprintf(buf, sizeof buf, po.agitate_after || po.agitate_before ? "%ds ~" : "%ds", po.pause_s);
    centered(buf, &inter_16, hex(MUTED), base + 78);
  }
  refresh_detail();
}

// A reading in the Scale / Water / Time box: the number, then a smaller unit, centred
// as a pair on a shared baseline. Falls back to a smaller font if it would not fit.
void set_stat(int i, const char *num, const char *unit) {
  lv_obj_t *n = d_stats[i][1], *u = d_stat_unit[i];
  set_text(n, num);
  set_text(u, unit);
  const int colw = D_LEFT_W / 3, gap = unit[0] ? 4 : 0;
  lv_obj_update_layout(n);
  lv_obj_update_layout(u);
  int wn = lv_obj_get_width(n);
  const int wu = unit[0] ? lv_obj_get_width(u) : 0;
  const lv_font_t *big = wn + gap + wu > colw - 16 ? &inter_20b : &inter_26b;
  if (lv_obj_get_style_text_font(n, 0) != big) {
    lv_obj_set_style_text_font(n, big, 0);
    lv_obj_update_layout(n);
    wn = lv_obj_get_width(n);
  }
  const int x0 = (colw - (wn + gap + wu)) / 2, base = D_STATS_H - 18;
  const lv_font_t *fs = &inter_16;
  lv_obj_set_pos(n, x0, base - (lv_font_get_line_height(big) - big->base_line));
  lv_obj_set_pos(u, x0 + wn + gap, base - (lv_font_get_line_height(fs) - fs->base_line));
}

void fmt_min_sec(char *buf, size_t n, uint32_t ms) {
  const uint32_t s = ms / 1000;
  snprintf(buf, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

void refresh_detail() {
  const PanelRecipe *pp = find(cur);
  if (!scr_detail || !pp) return;
  const xb::Recipe &r = pp->r;
  const bool mine_active = active == cur;
  const bool brewing = mine_active && (flow == Flow::Brewing || flow == Flow::Paused);
  char buf[32];
  const char *lbl_idle[3] = {"Dose", "Grind Size", "RPM"};
  const char *lbl_brew[3] = {"Scale", "Water", "Time"};
  for (int i = 0; i < 3; ++i) set_text(d_stats[i][0], (brewing ? lbl_brew : lbl_idle)[i]);
  if (brewing) {
    snprintf(buf, sizeof buf, "%.1f", weight_g);
    set_stat(0, buf, "g");
    snprintf(buf, sizeof buf, "%.0f", water_ml);
    set_stat(1, buf, "ml");
    fmt_min_sec(buf, sizeof buf, brew_start_ms ? millis() - brew_start_ms : 0);
    set_stat(2, buf, "");
  } else {
    snprintf(buf, sizeof buf, "%g", r.dose_g);
    set_stat(0, buf, "g");
    snprintf(buf, sizeof buf, "%d", r.grind_size);
    set_stat(1, buf, "");
    snprintf(buf, sizeof buf, "%d", r.rpm);
    set_stat(2, buf, "");
  }

  // bars fill with water as the brew goes
  for (int i = 0; i < d_ncols; ++i) {
    const double f = brewing ? fmax(0, fmin(1, (water_ml - d_cols[i].cum) / d_cols[i].vol)) : 0;
    const int bh = lv_obj_get_height(d_cols[i].bar), fh = (int)(bh * f);
    lv_obj_set_y(d_cols[i].fill, bh - fh);
    lv_obj_set_height(d_cols[i].fill, fh);
  }

  const char *label = "START", *icon = LV_SYMBOL_PLAY;
  const bool other_busy = active && !mine_active && flow != Flow::Idle;
  if (mine_active) {
    switch (flow) {
      case Flow::Loading: label = "LOADING"; icon = LV_SYMBOL_REFRESH; break;
      case Flow::Loaded: label = "READY"; break;
      case Flow::Starting: label = "STARTING"; icon = LV_SYMBOL_REFRESH; break;
      case Flow::Brewing: label = "PAUSE"; icon = LV_SYMBOL_PAUSE; break;
      case Flow::Paused: label = "RESUME"; icon = LV_SYMBOL_PLAY; break;
      default: break;
    }
  } else if (other_busy) {
    label = "BUSY";
  }
  set_text(d_start_lbl, label);
  set_text(d_start_icon, icon);
  lv_obj_set_width(d_start, brewing ? D_LEFT_W - D_BTN_H - G : D_LEFT_W);
  show(d_stop, brewing);
  const bool usable = link == LinkState::Connected && !other_busy;
  set_opa(d_start, usable ? LV_OPA_COVER : LV_OPA_50);
}

void on_start(lv_event_t *) {
  if (link != LinkState::Connected) return show_toast("Not connected to the xBloom");
  if (active == cur) {
    if (flow == Flow::Brewing) post(BleCmdType::Pause);
    else if (flow == Flow::Paused) post(BleCmdType::Resume);
    else if (flow == Flow::Loaded) show(modal, true);
    return;
  }
  if (flow != Flow::Idle) return show_toast("Another recipe is on the machine");
  const PanelRecipe *pp = find(cur);
  if (!pp) return;
  if (const char *err = xb::validate(pp->r)) return show_toast(err);
  BleCmd c;
  c.type = BleCmdType::Load;
  c.recipe = pp->r;
  active_copy = pp->r;
  if (!ble_link::post(c)) return;
  active = cur;
  flow = Flow::Loading;
  refresh_detail();
}

void on_stop(lv_event_t *) { post(BleCmdType::Stop); }

// ============================================================================
// GRINDER
// ============================================================================
const char *grind_use(int s) {
  if (s <= 12) return "Espresso";
  if (s <= 24) return "Moka Pot";
  if (s <= 32) return "AeroPress";
  if (s <= 55) return "Pourover\nCoffee Maker";
  if (s <= 70) return "French Press";
  return "Cold Brew";
}
ValueCard *gr_size_card;

void place_grind_use() {
  set_text(gr_use, grind_use(g_size));
  baseline_next_to(gr_use, gr_size_card->value, true, 20);
}

void grinder_back(lv_event_t *) {
  if (grinding) post(BleCmdType::GrinderStop);
  grinding = false;
  set_text(gr_play_icon, LV_SYMBOL_PLAY);
  post(BleCmdType::QuitGrinder);
  go_home_from_module();
}

void build_grinder() {
  scr_grinder = new_screen(BG_MOD);
  module_header(scr_grinder, Dots::Grinder, grinder_back);
  gr_weight = readout(scr_grinder, "0.0g", M);
  const int ch = (MOD_BOTTOM - MOD_TOP - G) / 2;
  gr_size_card = value_card(scr_grinder, M, MOD_TOP, MOD_LEFT_W, ch, "Grind size", "Level", GRINDER_FG,
                            GRINDER_FILL, &inter_96_num, 1, 80, 1, g_size, "%.0f", [](double v, bool rel) {
                              g_size = (int)v;
                              place_grind_use();
                              if (rel) post(BleCmdType::GrinderEnter, g_size, g_rpm);
                            });
  gr_use = text(gr_size_card->card, "", &inter_20, hex(GRINDER_FG));
  lv_obj_set_style_text_align(gr_use, LV_TEXT_ALIGN_RIGHT, 0);
  place_grind_use();
  value_card(scr_grinder, M, MOD_TOP + ch + G, MOD_LEFT_W, ch, "Grinder Speed", "RPM", GRINDER_FG, GRINDER_FILL,
             &inter_96_num, 60, 120, 10, g_rpm, "%.0f", [](double v, bool rel) {
               g_rpm = (int)v;
               if (rel) post(BleCmdType::GrinderEnter, g_size, g_rpm);
             });
  const int d = 136;
  round_button(scr_grinder, MOD_RIGHT_X + (MOD_RIGHT_W - d) / 2, MOD_TOP + (MOD_BOTTOM - MOD_TOP - d) / 2, d,
               LV_SYMBOL_PLAY, &lv_font_montserrat_28, &gr_play_icon, [](lv_event_t *) {
                 if (link != LinkState::Connected) return show_toast("Not connected to the xBloom");
                 grinding = !grinding;
                 post(grinding ? BleCmdType::GrinderStart : BleCmdType::GrinderStop, g_size, g_rpm);
                 set_text(gr_play_icon, grinding ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
               });
}

// ============================================================================
// BREWER
// ============================================================================
void set_pattern(uint8_t p, bool send) {
  b_pattern = p;
  const uint8_t order[3] = {xb::PAT_CENTERED, xb::PAT_CIRCULAR, xb::PAT_SPIRAL};
  for (int i = 0; i < 3; ++i) {
    const bool on = order[i] == p;
    lv_obj_set_style_bg_color(pat_btn[i], hex(on ? BREWER_SEL : SURFACE), 0);
    set_text_color(lv_obj_get_child(pat_btn[i], 1), hex(on ? BREWER_FG : 0xBBBBBB), 0);
  }
  if (send) post(BleCmdType::BrewerPattern, xb::pattern_to_byte(p));
}

void brewer_back(lv_event_t *) {
  if (pouring) post(BleCmdType::BrewerStop);
  pouring = false;
  set_text(br_play_icon, LV_SYMBOL_PLAY);
  post(BleCmdType::QuitBrewer);
  go_home_from_module();
}

void build_brewer() {
  scr_brewer = new_screen(BG_MOD);
  module_header(scr_brewer, Dots::Brewer, brewer_back);
  br_weight = readout(scr_brewer, "0.0g", M);
  br_timer = readout(scr_brewer, "00'00''", M + MOD_LEFT_W, true);
  const int gap = 14, ch = (MOD_BOTTOM - MOD_TOP - 2 * gap) / 3;
  value_card(scr_brewer, M, MOD_TOP, MOD_LEFT_W, ch, "Volume", "ml", BREWER_FG, BREWER_FILL, &inter_64b, 30, 500,
             5, b_volume, "%.0f", [](double v, bool) { b_volume = (int)v; });
  value_card(scr_brewer, M, MOD_TOP + ch + gap, MOD_LEFT_W, ch, "Temperature", "°C", BREWER_FG, BREWER_FILL,
             &inter_64b, 39, 96, 1, b_temp, "%.0f", [](double v, bool rel) {
               b_temp = (int)v;
               if (rel) post(BleCmdType::BrewerTemp, 0, 0, xb::temp_display_to_wire(b_temp));
             });
  value_card(scr_brewer, M, MOD_TOP + 2 * (ch + gap), MOD_LEFT_W, ch, "Flow rate", "ml/s", BREWER_FG, BREWER_FILL,
             &inter_64b, 3.0, 3.5, 0.1, b_flow, "%.1f", [](double v, bool) { b_flow = (float)v; });

  const uint8_t order[3] = {xb::PAT_CENTERED, xb::PAT_CIRCULAR, xb::PAT_SPIRAL};
  const char *names[3] = {"Centered", "Circular", "Spiral"};
  const int bh = 56, bgap = 12;
  for (int i = 0; i < 3; ++i) {
    pat_btn[i] = option_button(scr_brewer, MOD_RIGHT_X, MOD_TOP + i * (bh + bgap), MOD_RIGHT_W, bh);
    lv_obj_t *ic = pattern_icon(pat_btn[i], order[i], 28, hex(0xDDDDDD));
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_t *l = text(pat_btn[i], names[i], &inter_20b, hex(0xBBBBBB));
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 58, 0);
    on_click(pat_btn[i], [](lv_event_t *e) { set_pattern((uint8_t)(intptr_t)lv_event_get_user_data(e), true); },
             (void *)(intptr_t)order[i]);
  }
  set_pattern(b_pattern, false);
  const int d = 100;
  round_button(scr_brewer, MOD_RIGHT_X + (MOD_RIGHT_W - d) / 2, MOD_BOTTOM - d, d, LV_SYMBOL_PLAY,
               &lv_font_montserrat_28, &br_play_icon, [](lv_event_t *) {
                 if (link != LinkState::Connected) return show_toast("Not connected to the xBloom");
                 pouring = !pouring;
                 if (pouring) {
                   pour_start_ms = millis();
                   post(BleCmdType::BrewerStart, xb::pattern_to_byte(b_pattern), 0, b_flow, b_volume,
                        xb::temp_display_to_wire(b_temp));
                 } else {
                   post(BleCmdType::BrewerStop);
                 }
                 set_text(br_play_icon, pouring ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
               });
}

// ============================================================================
// SCALE
// ============================================================================
void render_scale() {
  char buf[16];
  if (unit == 1) snprintf(buf, sizeof buf, "%.2f", weight_g / 28.3495f);
  else snprintf(buf, sizeof buf, "%.1f", weight_g);
  set_text(sc_value, buf);
  set_text(sc_unit, unit == 0 ? "g" : unit == 1 ? "oz" : "ml");
  lv_obj_update_layout(sc_unit);
  // centre value + unit together
  lv_obj_align(sc_value, LV_ALIGN_CENTER, -(lv_obj_get_width(sc_unit) + 6) / 2, 0);
  baseline_next_to(sc_unit, sc_value, false, 6);
}

void set_unit(int u, bool send) {
  unit = u;
  for (int i = 0; i < 3; ++i) {
    lv_obj_set_style_bg_color(unit_btn[i], hex(i == u ? SCALE_SEL : SURFACE), 0);
    set_text_color(lv_obj_get_child(unit_btn[i], 0), hex(i == u ? SCALE_FG : 0xDDDDDD), 0);
  }
  render_scale();
  const xb::WeightUnit codes[3] = {xb::UNIT_G, xb::UNIT_OZ, xb::UNIT_ML};
  if (send) post(BleCmdType::WeightUnit, codes[u]);
}

void build_scale() {
  scr_scale = new_screen(BG_MOD);
  module_header(scr_scale, Dots::Scale, [](lv_event_t *) {
    post(BleCmdType::QuitScale);
    go_home_from_module();
  });
  sc_timer = readout(scr_scale, "00'00''", M);
  const int sw = 48;
  round_button(scr_scale, M + MOD_LEFT_W - sw, 96, sw, LV_SYMBOL_PLAY, &lv_font_montserrat_16, &sc_play_icon,
               [](lv_event_t *) {
                 if (sw_running) {
                   sw_elapsed += millis() - sw_start;
                   sw_running = false;
                 } else {
                   sw_elapsed = 0;  // a stopped timer restarts from zero
                   sw_start = millis();
                   sw_running = true;
                 }
                 set_text(sc_play_icon, sw_running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
               });
  lv_obj_t *card = box(scr_scale, M, MOD_TOP, MOD_LEFT_W, MOD_BOTTOM - MOD_TOP, BG_MOD, R);
  lv_obj_set_style_border_color(card, hex(SCALE_FG), 0);
  lv_obj_set_style_border_width(card, 2, 0);
  lv_obj_t *inner = box(card, 6, 6, MOD_LEFT_W - 12, MOD_BOTTOM - MOD_TOP - 12, SCALE_FILL, R - 6);
  sc_value = text(inner, "0.0", &inter_96_num, hex(0x5FB5A4));
  sc_unit = text(inner, "g", &inter_34b, hex(0x5FB5A4));
  const char *u[3] = {"g", "oz", "ml"};
  const int bh = 60, bgap = 12;
  for (int i = 0; i < 3; ++i) {
    unit_btn[i] = option_button(scr_scale, MOD_RIGHT_X, MOD_TOP + i * (bh + bgap), MOD_RIGHT_W, bh);
    lv_obj_center(text(unit_btn[i], u[i], &inter_26b, hex(0xDDDDDD)));
    on_click(unit_btn[i], [](lv_event_t *e) { set_unit((int)(intptr_t)lv_event_get_user_data(e), true); },
             (void *)(intptr_t)i);
  }
  const int d = 88;
  round_button(scr_scale, MOD_RIGHT_X + (MOD_RIGHT_W - d) / 2, MOD_BOTTOM - d, d, "T", &inter_34b, nullptr,
               [](lv_event_t *) { post(BleCmdType::Tare); });
  set_unit(0, false);
}

// ============================================================================
// LIBRARY: load / save / merge with the account
// ============================================================================
const char *LIB_FILE = "/recipes.json";

void assign_uid(PanelRecipe &p) {
  if (!p.uid) p.uid = next_uid++;
  else if (p.uid >= next_uid) next_uid = p.uid + 1;
}

void save_lib() {
  if (!storage::write(LIB_FILE, library::to_cache_json(lib))) Serial.println("[lib] save failed");
}

void load_lib() {
  std::vector<PanelRecipe> cached;
  std::string data;
  const bool have = storage::read(LIB_FILE, data) && library::from_cache_json(data.data(), data.size(), cached);
  lib.clear();
  for (PanelRecipe &p : cached)
    if (p.kind != KIND_STARTER) lib.push_back(p);
  if (!have) lib = recipes::seeds();  // first start: your recipes until you sign in
  for (PanelRecipe s : recipes::starters()) {
    for (const PanelRecipe &c : cached)
      if (c.kind == KIND_STARTER && strcmp(c.r.name, s.r.name) == 0) s.favorite = c.favorite;
    lib.push_back(s);
  }
  for (PanelRecipe &p : lib) assign_uid(p);
}

void rebuild_lists() {
  build_favorites();
  build_recipe_list();
}

// After a recipe changed or vanished: keep the recipe screen honest.
void detail_follow_library() {
  if (lv_screen_active() != scr_detail) return;
  if (!find(cur)) {
    go_home_from_detail();
    return;
  }
  if (!(active == cur && flow != Flow::Idle)) build_detail();
}

PanelRecipe *find_by_id(std::vector<PanelRecipe> &v, const char *id) {
  if (!*id) return nullptr;
  for (auto &p : v)
    if (strcmp(p.id, id) == 0) return &p;
  return nullptr;
}

// The account is the source of truth, except for edits made on the panel that
// haven't reached it yet (dirty): those win and are sent again.
void merge_account(std::vector<PanelRecipe> &account) {
  std::vector<PanelRecipe> out;
  std::vector<PanelRecipe> old = lib;
  for (PanelRecipe &c : account) {
    PanelRecipe *o = find_by_id(old, c.id);
    if (!o) {  // a seed or a panel-made recipe with the same name becomes this one
      for (auto &x : old)
        if (!x.id[0] && x.kind != KIND_STARTER && strcmp(x.r.name, c.r.name) == 0) {
          strlcpy(x.id, c.id, sizeof x.id);
          o = &x;
          break;
        }
    }
    if (o && o->dirty && o->kind != KIND_SEED) {
      PanelRecipe keep = *o;
      keep.kind = c.kind;
      strlcpy(keep.id, c.id, sizeof keep.id);
      out.push_back(keep);
      continue;
    }
    c.uid = o ? o->uid : 0;
    c.favorite = o ? o->favorite : false;
    out.push_back(c);
  }
  for (PanelRecipe &o : old) {
    if (o.kind == KIND_STARTER) continue;
    if (o.kind == KIND_SEED) continue;                              // replaced by the account copies
    if (o.id[0] && !find_by_id(out, o.id)) continue;                // deleted in the app
    if (!o.id[0] && o.dirty) out.push_back(o);                      // made on the panel, not sent yet
  }
  for (PanelRecipe &o : old)
    if (o.kind == KIND_STARTER) out.push_back(o);
  for (PanelRecipe &p : out) assign_uid(p);
  lib.swap(out);
  for (const PanelRecipe &p : lib)
    if (p.dirty && p.kind != KIND_STARTER && p.kind != KIND_SEED) cloud::push(p);
  save_lib();
  rebuild_lists();
  detail_follow_library();
}

// A recipe edited (or created) on the panel.
void store_recipe(PanelRecipe p) {
  p.dirty = true;
  if (p.kind == KIND_SEED || p.kind == KIND_STARTER) p.kind = KIND_MINE;
  PanelRecipe *slot = find(p.uid);
  if (slot) *slot = p;
  else {
    assign_uid(p);
    lib.insert(lib.begin(), p);
  }
  save_lib();
  if (cloud::state() == cloud::Acct::SignedIn) cloud::push(p);
  rebuild_lists();
}

void delete_recipe(uint32_t uid) {
  for (size_t i = 0; i < lib.size(); ++i)
    if (lib[i].uid == uid) {
      if (lib[i].id[0] && cloud::state() == cloud::Acct::SignedIn) cloud::remove(lib[i].id);
      lib.erase(lib.begin() + i);
      break;
    }
  save_lib();
  rebuild_lists();
}

void toggle_favorite(uint32_t uid) {
  PanelRecipe *p = find(uid);
  if (!p) return;
  p->favorite = !p->favorite;
  save_lib();
  build_favorites();
}

// ============================================================================
// CONFIRM DIALOG
// ============================================================================
lv_obj_t *cf_layer, *cf_title, *cf_text, *cf_ok_lbl;
void (*cf_action)() = nullptr;

void build_confirm() {
  cf_layer = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(cf_layer);
  lv_obj_set_size(cf_layer, W, H);
  lv_obj_set_style_bg_color(cf_layer, hex(0x000000), 0);
  lv_obj_set_style_bg_opa(cf_layer, LV_OPA_70, 0);
  lv_obj_add_flag(cf_layer, LV_OBJ_FLAG_CLICKABLE);
  const int dw = 456, dh = 228, pad = 28, bw = (dw - 2 * pad - G) / 2, bh = 56;
  lv_obj_t *dlg = box(cf_layer, 0, 0, dw, dh, 0x1C1C1E, 24);
  lv_obj_center(dlg);
  cf_title = text(dlg, "", &inter_26b, hex(TEXT));
  lv_obj_set_pos(cf_title, pad, pad);
  cf_text = text(dlg, "", &inter_16, hex(SUB));
  lv_label_set_long_mode(cf_text, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(cf_text, dw - 2 * pad);
  lv_obj_set_pos(cf_text, pad, pad + 44);
  lv_obj_t *no = pill(dlg, pad, dh - pad - bh, bw, bh, "Cancel", 0x2C2C2E, TEXT, [](lv_event_t *) { show(cf_layer, false); });
  (void)no;
  lv_obj_t *yes = pill(dlg, pad + bw + G, dh - pad - bh, bw, bh, "Delete", 0xFF453A, TEXT, [](lv_event_t *) {
    show(cf_layer, false);
    if (cf_action) cf_action();
  });
  cf_ok_lbl = lv_obj_get_child(yes, 0);
  show(cf_layer, false);
}

void confirm(const char *title, const char *body, const char *ok, void (*action)()) {
  set_text(cf_title, title);
  set_text(cf_text, body);
  set_text(cf_ok_lbl, ok);
  cf_action = action;
  lv_obj_move_foreground(cf_layer);
  show(cf_layer, true);
}

// ============================================================================
// RECIPE EDITOR
// ============================================================================
lv_obj_t *scr_edit, *ed_title, *ed_name, *ed_summary, *ed_delete, *ed_chips, *ed_cup_btn[4], *ed_pat_btn[3];
lv_obj_t *ed_agit_btn[2], *ed_remove_pour;
PanelRecipe ed;
uint32_t ed_uid = 0;  // 0 = new recipe
int ed_pour = 0;

enum Field { F_DOSE, F_GRIND, F_RPM, F_VOL, F_TEMP, F_WAIT, F_FLOW, F_COUNT };
lv_obj_t *ed_val[F_COUNT];
int ed_repeat = 0;

void refresh_editor();

double *field_ptr(Field f, double &tmp) {
  xb::Pour &q = ed.r.pours[ed_pour];
  switch (f) {
    case F_DOSE: return &ed.r.dose_g;
    case F_VOL: return &q.volume_ml;
    case F_TEMP: return &q.temp_c;
    case F_FLOW: return &q.flow;
    default: return &tmp;
  }
}

void adjust(Field f, int dir) {
  const int boost = ed_repeat > 20 ? 10 : ed_repeat > 6 ? 5 : 1;  // hold to go faster
  xb::Recipe &r = ed.r;
  xb::Pour &q = r.pours[ed_pour];
  double lo, hi;
  switch (f) {
    case F_DOSE:
      recipes::dose_range(r.cup_type, lo, hi);
      r.dose_g = fmin(hi, fmax(lo, r.dose_g + dir * 0.5 * (boost > 1 ? 2 : 1)));
      break;
    case F_GRIND: r.grind_size = (int)fmin(80, fmax(1, r.grind_size + dir * boost)); break;
    case F_RPM: r.rpm = (int)fmin(120, fmax(60, r.rpm + dir * 10)); break;
    case F_VOL: q.volume_ml = fmin(240, fmax(0, round(q.volume_ml) + dir * boost)); break;
    case F_TEMP: q.temp_c = fmin(98, fmax(40, round(q.temp_c) + dir * (boost > 1 ? 2 : 1))); break;
    case F_WAIT: q.pause_s = (uint8_t)fmin(59, fmax(0, (int)q.pause_s + dir * (boost > 1 ? 5 : 1))); break;
    case F_FLOW: q.flow = fmin(3.5, fmax(3.0, round((q.flow + dir * 0.1) * 10) / 10)); break;
    default: break;
  }
  refresh_editor();
}

void stepper_event(lv_event_t *e) {
  const intptr_t v = (intptr_t)lv_event_get_user_data(e);
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) ed_repeat = 0;
  if (code == LV_EVENT_LONG_PRESSED_REPEAT) ++ed_repeat;
  if (code == LV_EVENT_PRESSED || code == LV_EVENT_LONG_PRESSED_REPEAT) adjust((Field)(v >> 1), (v & 1) ? 1 : -1);
}

// [ −   label / value   + ]
void stepper(lv_obj_t *parent, int x, int y, int w, int h, const char *label, Field f) {
  lv_obj_t *c = box(parent, x, y, w, h, 0x1C1C1E, R_SM);
  lv_obj_t *l = text(c, label, &inter_14, hex(MUTED));
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 8);
  ed_val[f] = text(c, "", &inter_26b, hex(TEXT));
  lv_obj_align(ed_val[f], LV_ALIGN_BOTTOM_MID, 0, -8);
  for (int side = 0; side < 2; ++side) {
    lv_obj_t *b = group(c, side ? w - 52 : 0, 0, 52, h);
    lv_obj_t *t = text(b, side ? LV_SYMBOL_PLUS : LV_SYMBOL_MINUS, &lv_font_montserrat_20, hex(SUB));
    lv_obj_center(t);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(b, hex(0x2C2C2E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    void *ud = (void *)(intptr_t)((f << 1) | side);
    lv_obj_add_event_cb(b, stepper_event, LV_EVENT_PRESSED, ud);
    lv_obj_add_event_cb(b, stepper_event, LV_EVENT_LONG_PRESSED_REPEAT, ud);
  }
}

void select_option(lv_obj_t **btns, int n, int sel, uint32_t on_bg = 0x3A3A3C) {
  for (int i = 0; i < n; ++i) {
    lv_obj_set_style_bg_color(btns[i], hex(i == sel ? on_bg : 0x1C1C1E), 0);
    lv_obj_t *l = lv_obj_get_child(btns[i], lv_obj_get_child_count(btns[i]) - 1);
    set_text_color(l, hex(i == sel ? TEXT : MUTED), 0);
  }
}

void build_pour_chips() {
  lv_obj_clean(ed_chips);
  const int cw = 36, gap = 4;
  for (int i = 0; i < ed.r.n_pours + (ed.r.n_pours < xb::MAX_POURS ? 1 : 0); ++i) {
    const bool add = i == ed.r.n_pours;
    lv_obj_t *c = box(ed_chips, i * (cw + gap), 0, cw, 40, add ? 0x2C2C2E : i == ed_pour ? 0xFFFFFF : 0x1C1C1E, 10);
    char n[4];
    snprintf(n, sizeof n, "%d", i + 1);
    lv_obj_t *t = text(c, add ? LV_SYMBOL_PLUS : n, add ? &lv_font_montserrat_16 : &inter_16,
                       hex(!add && i == ed_pour ? 0x000000 : TEXT));
    lv_obj_center(t);
    pressable(c);
    on_click(c, [](lv_event_t *e) {
      const int i = (int)(intptr_t)lv_event_get_user_data(e);
      if (i == ed.r.n_pours) {  // add a pour like the last one
        ed.r.pours[i] = ed.r.pours[i - 1];
        ed.pour_names[i][0] = 0;
        ++ed.r.n_pours;
      }
      ed_pour = i;
      refresh_editor();
    }, (void *)(intptr_t)i);
  }
}

void refresh_editor() {
  const xb::Recipe &r = ed.r;
  const xb::Pour &q = r.pours[ed_pour];
  char buf[48];
  set_text(ed_name, r.name[0] ? r.name : "Untitled");
  snprintf(buf, sizeof buf, "%gg", r.dose_g);
  set_text(ed_val[F_DOSE], buf);
  snprintf(buf, sizeof buf, "%d", r.grind_size);
  set_text(ed_val[F_GRIND], buf);
  snprintf(buf, sizeof buf, "%d", r.rpm);
  set_text(ed_val[F_RPM], buf);
  snprintf(buf, sizeof buf, "%.0f ml", q.volume_ml);
  set_text(ed_val[F_VOL], buf);
  snprintf(buf, sizeof buf, "%.0f°C", q.temp_c);
  set_text(ed_val[F_TEMP], buf);
  snprintf(buf, sizeof buf, "%ds", q.pause_s);
  set_text(ed_val[F_WAIT], buf);
  snprintf(buf, sizeof buf, "%.1f", q.flow);
  set_text(ed_val[F_FLOW], buf);
  snprintf(buf, sizeof buf, "1:%g  |  %.0f ml", library::ratio_denominator(r), r.total_water());
  set_text(ed_summary, buf);
  select_option(ed_cup_btn, 4, r.cup_type - 1);
  const uint8_t order[3] = {xb::PAT_CENTERED, xb::PAT_CIRCULAR, xb::PAT_SPIRAL};
  for (int i = 0; i < 3; ++i)
    if (order[i] == q.pattern) select_option(ed_pat_btn, 3, i);
  lv_obj_set_style_bg_color(ed_agit_btn[0], hex(q.agitate_before ? 0x3A3A3C : 0x1C1C1E), 0);
  lv_obj_set_style_bg_color(ed_agit_btn[1], hex(q.agitate_after ? 0x3A3A3C : 0x1C1C1E), 0);
  set_text_color(lv_obj_get_child(ed_agit_btn[0], 0), hex(q.agitate_before ? TEXT : MUTED), 0);
  set_text_color(lv_obj_get_child(ed_agit_btn[1], 0), hex(q.agitate_after ? TEXT : MUTED), 0);
  show(ed_remove_pour, r.n_pours > 1);
  build_pour_chips();
}

void editor_save(lv_event_t *) {
  if (const char *err = xb::validate(ed.r)) return show_toast(err);
  store_recipe(ed);
  cur = ed.uid ? ed.uid : lib.front().uid;
  if (cloud::state() != cloud::Acct::SignedIn) show_toast("Saved on the panel. Sign in to save it to your account too.");
  open_recipe(cur, Anim::PopRight);
}

void build_editor() {
  scr_edit = new_screen(BG);
  back_button(scr_edit, LV_SYMBOL_LEFT, [](lv_event_t *) {
    if (ed_uid && find(ed_uid)) open_recipe(ed_uid, Anim::PopRight);
    else load(scr_home, Anim::PopRight);
  });
  ed_title = text(scr_edit, "Edit recipe", &inter_26b, hex(TEXT));
  lv_obj_set_pos(ed_title, M + 44, SB + 10);
  pill(scr_edit, W - M - 110, SB + 4, 110, 44, "Save", 0xFFFFFF, 0x000000, editor_save);
  ed_delete = pill(scr_edit, W - M - 110 - 12 - 110, SB + 4, 110, 44, "Delete", 0x2C2C2E, 0xFF6B6B, [](lv_event_t *) {
    confirm("Delete recipe?", cloud::state() == cloud::Acct::SignedIn && ed.id[0]
                                  ? "It will also be deleted from your xBloom account."
                                  : "It will be removed from the panel.",
            "Delete", [] {
              delete_recipe(ed_uid);
              load(scr_home, Anim::PopRight);
            });
  });

  // ---- left column: name, cup, dose / grind / rpm
  const int lx = M, lw = 352, top = 96;
  lv_obj_t *nm = box(scr_edit, lx, top, lw, 52, 0x1C1C1E, R_SM);
  ed_name = text(nm, "", &inter_20b, hex(TEXT));
  lv_label_set_long_mode(ed_name, LV_LABEL_LONG_DOT);
  lv_obj_set_width(ed_name, lw - 60);
  lv_obj_align(ed_name, LV_ALIGN_LEFT_MID, 16, 0);
  lv_obj_t *pen = text(nm, LV_SYMBOL_EDIT, &lv_font_montserrat_16, hex(MUTED));
  lv_obj_align(pen, LV_ALIGN_RIGHT_MID, -16, 0);
  pressable(nm);
  on_click(nm, [](lv_event_t *) {
    open_text_entry("Recipe name", ed.r.name, "Name", false, "Done", scr_edit, [](const char *t) {
      strlcpy(ed.r.name, t, sizeof ed.r.name);
      refresh_editor();
      load(scr_edit, Anim::PopRight);
    });
  });
  const char *cups[4] = {"xPod", "Omni", "Other", "Tea"};
  const int cw = (lw - 3 * 6) / 4;
  for (int i = 0; i < 4; ++i) {
    ed_cup_btn[i] = box(scr_edit, lx + i * (cw + 6), top + 60, cw, 44, 0x1C1C1E, 12);
    lv_obj_center(text(ed_cup_btn[i], cups[i], &inter_16, hex(MUTED)));
    pressable(ed_cup_btn[i]);
    on_click(ed_cup_btn[i], [](lv_event_t *e) {
      ed.r.cup_type = (int)(intptr_t)lv_event_get_user_data(e);
      double lo, hi;
      recipes::dose_range(ed.r.cup_type, lo, hi);
      ed.r.dose_g = fmin(hi, fmax(lo, ed.r.dose_g));
      refresh_editor();
    }, (void *)(intptr_t)(i + 1));
  }
  const int sh = 72, sy = top + 112;
  stepper(scr_edit, lx, sy, lw, sh, "Dose", F_DOSE);
  stepper(scr_edit, lx, sy + sh + 8, lw, sh, "Grind size", F_GRIND);
  stepper(scr_edit, lx, sy + 2 * (sh + 8), lw, sh, "RPM", F_RPM);

  // ---- right column: pours
  const int rx = lx + lw + 24, rw = W - M - rx;
  ed_chips = group(scr_edit, rx, top, rw, 40);
  const int hw = (rw - 8) / 2, py = top + 48;
  stepper(scr_edit, rx, py, hw, sh, "Volume", F_VOL);
  stepper(scr_edit, rx + hw + 8, py, hw, sh, "Temperature", F_TEMP);
  stepper(scr_edit, rx, py + sh + 8, hw, sh, "Wait after", F_WAIT);
  stepper(scr_edit, rx + hw + 8, py + sh + 8, hw, sh, "Flow rate", F_FLOW);
  const uint8_t order[3] = {xb::PAT_CENTERED, xb::PAT_CIRCULAR, xb::PAT_SPIRAL};
  const char *pn[3] = {"Centered", "Circular", "Spiral"};
  const int pw = (rw - 2 * 6) / 3, pyy = py + 2 * (sh + 8);
  for (int i = 0; i < 3; ++i) {
    ed_pat_btn[i] = box(scr_edit, rx + i * (pw + 6), pyy, pw, 44, 0x1C1C1E, 12);
    lv_obj_t *ic = pattern_icon(ed_pat_btn[i], order[i], 20, hex(0xDDDDDD));
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *l = text(ed_pat_btn[i], pn[i], &inter_14, hex(MUTED));
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 36, 0);
    pressable(ed_pat_btn[i]);
    on_click(ed_pat_btn[i], [](lv_event_t *e) {
      ed.r.pours[ed_pour].pattern = (uint8_t)(intptr_t)lv_event_get_user_data(e);
      refresh_editor();
    }, (void *)(intptr_t)order[i]);
  }
  const char *ag[2] = {"Agitate before", "Agitate after"};
  for (int i = 0; i < 2; ++i) {
    ed_agit_btn[i] = box(scr_edit, rx + i * (hw + 8), pyy + 52, hw, 40, 0x1C1C1E, 12);
    lv_obj_center(text(ed_agit_btn[i], ag[i], &inter_14, hex(MUTED)));
    pressable(ed_agit_btn[i]);
    on_click(ed_agit_btn[i], [](lv_event_t *e) {
      xb::Pour &q = ed.r.pours[ed_pour];
      if ((intptr_t)lv_event_get_user_data(e)) q.agitate_after = !q.agitate_after;
      else q.agitate_before = !q.agitate_before;
      refresh_editor();
    }, (void *)(intptr_t)i);
  }
  ed_summary = text(scr_edit, "", &inter_16, hex(SUB));
  lv_obj_set_pos(ed_summary, rx + 4, H - M - 26);
  ed_remove_pour = text(scr_edit, "Remove this pour", &inter_16, hex(0xFF6B6B));
  lv_obj_align(ed_remove_pour, LV_ALIGN_TOP_RIGHT, -M - 4, H - M - 26);
  lv_obj_set_ext_click_area(ed_remove_pour, 12);
  on_click(ed_remove_pour, [](lv_event_t *) {
    xb::Recipe &r = ed.r;
    if (r.n_pours <= 1) return;
    for (int i = ed_pour; i < r.n_pours - 1; ++i) {
      r.pours[i] = r.pours[i + 1];
      memcpy(ed.pour_names[i], ed.pour_names[i + 1], sizeof ed.pour_names[0]);
    }
    --r.n_pours;
    ed.pour_names[r.n_pours][0] = 0;
    if (ed_pour >= r.n_pours) ed_pour = r.n_pours - 1;
    refresh_editor();
  });
}

void open_editor(uint32_t uid) {
  const PanelRecipe *p = uid ? find(uid) : nullptr;
  if (p) {
    ed = *p;
    if (p->kind == KIND_STARTER) {  // a starter is copied into a new recipe of your own
      ed.uid = 0;
      ed.id[0] = 0;
      ed.kind = KIND_MINE;
      ed.favorite = false;
    }
  } else {
    ed = recipes::starters().front();
    strlcpy(ed.r.name, "New recipe", sizeof ed.r.name);
    ed.kind = KIND_MINE;
    ed.uid = 0;
    ed.color = CARDS[next_uid % N_CARDS];
  }
  ed_uid = ed.uid;
  ed_pour = 0;
  set_text(ed_title, ed_uid ? "Edit recipe" : "New recipe");
  show(ed_delete, ed_uid != 0);
  refresh_editor();
  load(scr_edit, Anim::PushLeft);
}

// ============================================================================
// ACCOUNT
// ============================================================================
lv_obj_t *scr_account, *ac_title, *ac_sub, *ac_msg, *ac_primary, *ac_primary_lbl, *ac_signout;
char ac_email[96];
cloud::Acct last_acct = (cloud::Acct)255;

void refresh_account() {
  const cloud::Acct s = cloud::state();
  char buf[128];
  if (s == cloud::Acct::SignedIn) {
    set_text(ac_title, cloud::email());
    int mine = 0, shared = 0;
    for (auto &p : lib) {
      if (p.id[0] && p.kind == KIND_MINE) ++mine;
      if (p.kind == KIND_SHARED) ++shared;
    }
    struct tm t;
    char when[32] = "not yet";
    const time_t ls = cloud::last_sync();
    if (ls && localtime_r(&ls, &t)) {
      int h = t.tm_hour % 12;
      snprintf(when, sizeof when, "at %d:%02d %s", h ? h : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
    }
    snprintf(buf, sizeof buf, "%d created  |  %d shared\nLast synced %s%s", mine, shared, when,
             cloud::busy() ? "  (syncing...)" : "");
    set_text(ac_sub, buf);
    set_text(ac_primary_lbl, cloud::busy() ? "Syncing..." : "Sync now");
  } else {
    set_text(ac_title, s == cloud::Acct::SigningIn ? "Signing in..." : "Not signed in");
    set_text(ac_sub, "Sign in with your xBloom app email and password to bring your recipes to the panel. "
                              "Edits sync both ways.");
    set_text(ac_primary_lbl, "Sign in");
  }
  set_text(ac_msg, cloud::message());
  show(ac_signout, s == cloud::Acct::SignedIn);
}

void build_account() {
  scr_account = new_screen(BG);
  sub_header(scr_account, "xBloom account", back_to_settings);
  lv_obj_t *card = box(scr_account, M, 96, W - 2 * M, 224, 0x1C1C1E, R_SM);
  ac_title = text(card, "", &inter_26b, hex(TEXT));
  lv_obj_set_pos(ac_title, 24, 22);
  ac_sub = text(card, "", &inter_20, hex(SUB));
  lv_label_set_long_mode(ac_sub, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ac_sub, W - 2 * M - 48);
  lv_obj_set_style_text_line_space(ac_sub, 6, 0);
  lv_obj_set_pos(ac_sub, 24, 68);
  ac_msg = text(card, "", &inter_16, hex(0xFF6B6B));
  lv_label_set_long_mode(ac_msg, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ac_msg, W - 2 * M - 48);
  lv_obj_set_pos(ac_msg, 24, 170);
  ac_primary = pill(scr_account, M, 336, 260, 56, "Sign in", 0xFFFFFF, 0x000000, [](lv_event_t *) {
    if (cloud::state() == cloud::Acct::SignedIn) {
      cloud::sync_now();
      return;
    }
    if (net::state() != net::Wifi::Connected) return show_toast("Connect to Wi-Fi first (Settings > Wi-Fi)");
    open_text_entry("Email", cloud::email(), "you@example.com", false, "Next", scr_account, [](const char *t) {
      strlcpy(ac_email, t, sizeof ac_email);
      open_text_entry("Password", "", "xBloom password", true, "Sign in", scr_account, [](const char *pw) {
        cloud::sign_in(ac_email, pw);
        refresh_account();
        load(scr_account, Anim::PopRight);
      });
    });
  });
  ac_primary_lbl = lv_obj_get_child(ac_primary, 0);
  ac_signout = pill(scr_account, M + 260 + G, 336, 200, 56, "Sign out", 0x2C2C2E, 0xFF6B6B, [](lv_event_t *) {
    confirm("Sign out?", "Your account's recipes will be removed from the panel. Recipes made on the panel stay.",
            "Sign out", [] { cloud::sign_out(); });
  });
  lv_obj_t *note = text(scr_account, "The panel saves your password so it can sign in again by itself.", &inter_16,
                        hex(MUTED));
  lv_obj_set_pos(note, M, 412);
}

void open_account(lv_event_t *) {
  refresh_account();
  load(scr_account, Anim::PushLeft);
}

void handle_cloud(const cloud::Event &e) {
  switch (e.type) {
    case cloud::Ev::Synced:
      merge_account(*e.list);
      delete e.list;
      break;
    case cloud::Ev::SyncFailed:
      if (lv_screen_active() == scr_account) refresh_account();
    refresh_sync_row();
      break;
    case cloud::Ev::Pushed:
      if (PanelRecipe *p = find(e.uid)) {
        strlcpy(p->id, e.id, sizeof p->id);
        p->dirty = false;
        save_lib();
        build_recipe_list();
      }
      break;
    case cloud::Ev::PushFailed: {
      char buf[160];
      snprintf(buf, sizeof buf, "Saved on the panel, but not to your account: %s", e.msg);
      show_toast(buf);
      break;
    }
    case cloud::Ev::RemoveFailed: {
      char buf[160];
      snprintf(buf, sizeof buf, "Couldn't delete it from your account: %s", e.msg);
      show_toast(buf);
      break;
    }
    case cloud::Ev::Removed: break;
    case cloud::Ev::Auth:
      if (cloud::state() == cloud::Acct::SignedOut) {  // drop the account's recipes, keep panel-made ones
        std::vector<PanelRecipe> keep;
        for (auto &p : lib)
          if (p.kind == KIND_STARTER || (!p.id[0] && p.kind != KIND_SHARED)) keep.push_back(p);
        lib.swap(keep);
        save_lib();
        rebuild_lists();
        detail_follow_library();
      }
      if (cloud::state() == cloud::Acct::BadLogin && e.msg[0]) show_toast(e.msg);
      break;
  }
}

// ============================================================================
// live updates
// ============================================================================
void render_weight() {
  char buf[16];
  snprintf(buf, sizeof buf, "%.1fg", weight_g);
  set_text(gr_weight, buf);
  set_text(br_weight, buf);
  render_scale();
}

void set_link(LinkState s, const char *name) {
  link = s;
  if (name && *name) strncpy(machine_name, name, sizeof machine_name - 1);
  set_text(home_name, machine_name);
  if (st_machine_val) set_text(st_machine_val, s == LinkState::Connected ? machine_name : "Not connected");
  const bool ok = s == LinkState::Connected;
  show(home_spin, !ok);
  show(home_dot, ok);
  set_text(home_status, ok ? "Connected" : s == LinkState::Connecting ? "Connecting" : "Searching");
  // one row:  XBLOOM 381 >   ● Connected
  baseline_next_to(home_chev, home_name, false, 8);
  lv_obj_update_layout(home_chev);
  lv_obj_align_to(home_dot, home_chev, LV_ALIGN_OUT_RIGHT_MID, 22, 0);
  lv_obj_align_to(home_spin, home_chev, LV_ALIGN_OUT_RIGHT_MID, 19, 0);
  baseline_next_to(home_status, home_name, false, 0);
  lv_obj_update_layout(home_dot);
  lv_obj_set_x(home_status, lv_obj_get_x(home_dot) + 10 + 8);
  set_text_color(sb_bt, hex(ok ? TEXT : MUTED), 0);
  if (!ok) {
    if (flow != Flow::Idle) { flow = Flow::Idle; active = 0; dotfx::brew_hide(); intro_for = 0; }
    grinding = pouring = false;
  }
  refresh_detail();
}

// The brewing scene (dot dripper + carafe) runs for the whole brew.
uint32_t water_seen_ms = 0;   // last time the poured volume went up
float last_water = 0;
const char *pattern_name(uint8_t p) {
  return p == xb::PAT_CENTERED ? "Centered" : p == xb::PAT_CIRCULAR ? "Circular" : "Spiral";
}
void update_brew_scene() {
  if (!dotfx::brew_showing()) return;
  const PanelRecipe *p = find(active);
  if (!p) return;
  if (water_ml > last_water + 0.4f) { last_water = water_ml; water_seen_ms = millis(); }
  dotfx::BrewView v;
  v.water_ml = water_ml;
  v.total_ml = p->r.total_water();
  v.grams = weight_g;
  v.ms = brew_start_ms ? millis() - brew_start_ms : 0;
  v.paused = flow == Flow::Paused;
  v.pouring = water_seen_ms && millis() - water_seen_ms < 1500;
  char title[32], sub[48];
  const int i = pour_idx >= 0 && pour_idx < p->r.n_pours ? pour_idx : -1;
  if (i < 0) {
    strcpy(title, "Starting");
    snprintf(sub, sizeof sub, "%d pours  |  %.0f ml", p->r.n_pours, p->r.total_water());
  } else {
    char nm[24];
    strlcpy(title, library::pour_name(*p, i, nm), sizeof title);
    const xb::Pour &po = p->r.pours[i];
    snprintf(sub, sizeof sub, "%.0f\xC2\xB0" "C  |  %s  |  %d of %d", po.temp_c, pattern_name(po.pattern), i + 1, p->r.n_pours);
  }
  v.pour_title = title;
  v.pour_sub = sub;
  dotfx::brew_update(v);
}
void brew_started() {
  if (!active || intro_for == active) return;
  intro_for = active;
  const PanelRecipe *p = find(active);
  if (!p) return;
  dotfx::brew_show(p->r.name,
                   [] { post(flow == Flow::Paused ? BleCmdType::Resume : BleCmdType::Pause); },
                   [] { post(BleCmdType::Stop); });
  water_seen_ms = 0;
  last_water = 0;
  update_brew_scene();
  wake_req = true;
}

void brew_ended() {
  intro_for = 0;
  dotfx::brew_hide();
  flow = Flow::Idle;
  active = 0;
  brew_start_ms = 0;
  pour_idx = -1;
  refresh_detail();
}

void handle(const UiEvent &e) {
  switch (e.type) {
    case UiEvType::Link: set_link((LinkState)e.i, e.msg); break;
    case UiEvType::Weight:
      weight_g = e.f;
      if (fabsf(weight_g - wake_ref_weight) >= 2.0f) {  // cup placed or lifted
        wake_ref_weight = weight_g;
        wake_req = true;
      }
      render_weight();
      break;
    case UiEvType::Water:
      // At the end of a brew the machine reports the total in thousandths of a ml.
      water_ml = e.f > 5000 ? e.f / 1000.0f : e.f;
      break;
    case UiEvType::PourIndex: pour_idx = e.i; break;
    case UiEvType::Activity: {
      wake_req = true;  // something happened on the machine (knob, brew phase)
      const bool brewing = e.i == 34 || e.i == 30 || e.i == 35 || e.i == 16;
      if (brewing && active && (flow == Flow::Loaded || flow == Flow::Starting)) {
        flow = Flow::Brewing;
        if (!brew_start_ms) brew_start_ms = millis();
        brew_started();
      }
      if (e.i == 1 && active && (flow == Flow::Brewing || flow == Flow::Paused)) brew_ended();
      refresh_detail();
      break;
    }
    case UiEvType::BrewDone: {
      const PanelRecipe *p = find(active);
      const float ml = water_ml > 0 ? water_ml : (p ? p->r.total_water() : 0);
      dotfx::finish_play(p ? p->r.name : "Your coffee", weight_g, ml, brew_start_ms ? millis() - brew_start_ms : 0);
      wake_req = true;
      brew_ended();
      break;
    }
    case UiEvType::BrewAbandoned: brew_ended(); break;
    case UiEvType::BrewPaused: if (flow == Flow::Brewing) flow = Flow::Paused; refresh_detail(); break;
    case UiEvType::BrewResumed: if (flow == Flow::Paused) flow = Flow::Brewing; refresh_detail(); break;
    case UiEvType::Fault: show_toast(e.msg); break;
    case UiEvType::Sleep: break;
    case UiEvType::PodScanned: wake_req = true; break;
    case UiEvType::CmdResult: {
      const BleCmdType t = (BleCmdType)e.i;
      if (!e.ok) {
        show_toast(e.msg[0] ? e.msg : "Command failed");
        if (t == BleCmdType::Load) brew_ended();
        if (t == BleCmdType::Execute) flow = Flow::Loaded;
        if (t == BleCmdType::GrinderStart) { grinding = false; set_text(gr_play_icon, LV_SYMBOL_PLAY); }
        if (t == BleCmdType::BrewerStart) { pouring = false; set_text(br_play_icon, LV_SYMBOL_PLAY); }
        refresh_detail();
        break;
      }
      if (t == BleCmdType::Load) {
        flow = Flow::Loaded;
        if (lv_screen_active() == scr_detail && cur == active) show(modal, true);
      } else if (t == BleCmdType::Execute) {
        flow = Flow::Brewing;
        water_ml = 0;
        brew_start_ms = millis();
        brew_started();
      } else if (t == BleCmdType::Stop) {
        brew_ended();
      }
      refresh_detail();
      break;
    }
  }
}

}  // namespace

// ============================================================================
namespace ui {

void begin() {
  storage::begin();
  load_lib();
  build_home_screen();
  build_grinder();
  build_brewer();
  build_scale();
  build_wifi();
  build_text_entry();
  build_editor();
  build_account();
  build_update();
  build_status_bar();
  build_modal();
  build_confirm();
  build_clock();

  toast = box(lv_layer_top(), 0, 0, 600, 0, 0x2C2C2E, R_SM);
  lv_obj_set_height(toast, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_hor(toast, 20, 0);
  lv_obj_set_style_pad_ver(toast, 14, 0);
  lv_obj_set_style_shadow_width(toast, 24, 0);
  lv_obj_set_style_shadow_opa(toast, LV_OPA_50, 0);
  lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, SB + 8);
  toast_lbl = text(toast, "", &inter_20, hex(TEXT));
  lv_label_set_long_mode(toast_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(toast_lbl, 560);
  on_click(toast, [](lv_event_t *) { show(toast, false); });
  show(toast, false);
  toast_timer = lv_timer_create([](lv_timer_t *t) { show(toast, false); lv_timer_pause(t); }, 4000, nullptr);
  lv_timer_pause(toast_timer);

  lv_screen_load(scr_home);
  set_link(LinkState::Scanning, nullptr);
}

void check_idle_home();
void tick() {
  UiEvent e;
  int n = 0;
  while (n++ < 24 && ble_link::poll(e)) handle(e);
  cloud::Event ce;
  while (cloud::poll(ce)) handle_cloud(ce);

  static uint32_t last = 0;
  if (millis() - last > 200) {
    last = millis();
    char buf[16];
    fmt_time(buf, sizeof buf, pouring ? millis() - pour_start_ms : 0);
    set_text(br_timer, buf);
    fmt_time(buf, sizeof buf, sw_elapsed + (sw_running ? millis() - sw_start : 0));
    set_text(sc_timer, buf);
    if (lv_screen_active() == scr_detail && active == cur) refresh_detail();
    update_brew_scene();
    check_idle_home();
    refresh_net();
    const cloud::Acct as = cloud::state();
    set_text(st_acc_val, as == cloud::Acct::SignedIn    ? cloud::email()
                                  : as == cloud::Acct::SigningIn ? "Signing in..."
                                                                 : "Not signed in");
    if (lv_screen_active() == scr_account) refresh_account();
    // a manual pour stops by itself at its volume; reset the button after it should have finished
    if (pouring && millis() - pour_start_ms > (uint32_t)((b_volume / b_flow + 8) * 1000)) {
      pouring = false;
      set_text(br_play_icon, LV_SYMBOL_PLAY);
    }
  }
}

// Nothing touched for 30 s and the machine idle: back to Home (xBloom tab).
const uint32_t HOME_AFTER_MS = 30 * 1000;
void go_home_idle() {
  dotfx::finish_hide();
  show(modal, false);
  show(cf_layer, false);
  show(toast, false);
  lv_obj_t *s = lv_screen_active();
  if (s == scr_grinder) grinder_back(nullptr);
  else if (s == scr_brewer) brewer_back(nullptr);
  else if (s == scr_scale) { post(BleCmdType::QuitScale); load(scr_home, Anim::None); }
  else if (s == scr_update) { net::update_page(false); load(scr_home, Anim::None); }
  else if (s != scr_home) load(scr_home, Anim::None);
  set_tab(0);
}
void check_idle_home() {
  if (busy() || dotfx::playing()) return;
  if (lv_display_get_inactive_time(nullptr) < HOME_AFTER_MS) return;
  const bool at_home = lv_screen_active() == scr_home && !lv_obj_has_flag(page_home, LV_OBJ_FLAG_HIDDEN);
  if (at_home && !dotfx::finish_showing()) return;
  go_home_idle();
}

bool busy() {
  return flow != Flow::Idle || grinding || pouring || sw_running || net::update_state() == net::Upd::Receiving;
}

void show_clock(bool on) {
  if (on) {
    refresh_clock();
    lv_obj_move_foreground(clock_layer);
  }
  show(clock_layer, on);
}

bool take_wake() {
  const bool w = wake_req;
  wake_req = false;
  return w;
}

#ifdef UI_SIM
// Desktop preview only: jump straight to a screen.
void sim_show(int which, int recipe) {
  switch (which) {
    case 0: lv_screen_load(scr_home); set_tab(0); break;
    case 1: lv_screen_load(scr_home); set_tab(1); break;
    case 2: open_recipe(lib[recipe].uid, Anim::None); break;
    case 3: open_recipe(lib[recipe].uid, Anim::None); active = lib[recipe].uid; flow = Flow::Brewing; water_ml = 95; weight_g = 88.4f;
            brew_start_ms = millis() - 74000; refresh_detail(); break;
    case 4: lv_screen_load(scr_grinder); break;
    case 5: lv_screen_load(scr_brewer); break;
    case 6: lv_screen_load(scr_scale); break;
    case 7: show(modal, true); break;
    case 8: show(modal, false); break;
    case 9: lv_screen_load(scr_home); set_tab(2); break;
    case 10: open_wifi(); break;
    case 11: open_password("Home Wi-Fi"); break;
    case 12: open_update(); break;
    case 13: show_clock(true); break;
    case 14: show_clock(false); break;
    case 15: open_editor(lib[recipe].uid); break;
    case 16: lv_screen_load(scr_home); set_tab(2); open_account(); break;
    case 17: lv_screen_load(scr_home); set_tab(1); set_seg(1); break;
    case 18: lv_screen_load(scr_home); set_tab(1); set_seg(0); break;
    case 19: confirm("Delete recipe?", "It will also be deleted from your xBloom account.", "Delete", nullptr); break;
    case 20: show(cf_layer, false); break;
    case 21: {  // brew start animation over the brewing screen
      open_recipe(lib[recipe].uid, Anim::None); active = lib[recipe].uid; flow = Flow::Brewing;
      water_ml = 0; weight_g = 0; brew_start_ms = millis(); intro_for = 0; refresh_detail(); brew_started(); break;
    }
    case 26: brew_ended(); break;
    case 27: lv_screen_load(scr_home); set_tab(2); lv_dropdown_open(st_sleep_dd); break;
    case 22: dotfx::finish_play(lib[recipe].r.name, 141.3f, 220, 181000); break;
    case 23: clock_color = dotfx::WHITE; show_clock(true); break;
    case 24: clock_color = dotfx::BLUE; show_clock(true); break;
    case 25: open_recipe(lib[recipe].uid, Anim::None); active = lib[recipe].uid; flow = Flow::Brewing;
             water_ml = 220000 / 1000.0f; weight_g = 141.3f; brew_start_ms = millis() - 181000; refresh_detail(); break;
  }
}
#endif

}  // namespace ui
