#include "dotfx.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_theme.h"

namespace dotfx {

const uint32_t BLUE = 0x3F9BFF;
const uint32_t WHITE = 0xF2F2F2;
const uint32_t COFFEE = 0x9A6236;

namespace {

lv_color_t hexc(uint32_t c) { return lv_color_hex(c); }
float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float smooth(float a, float b, float x) {
  const float t = clamp01((x - a) / (b - a));
  return t * t * (3 - 2 * t);
}
uint32_t hash2(int a, int b) {
  uint32_t h = (uint32_t)a * 374761393u + (uint32_t)b * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

// ---------------------------------------------------------------------------
// field object
// ---------------------------------------------------------------------------
void draw_cb(lv_event_t *e) {
  lv_obj_t *o = (lv_obj_t *)lv_event_get_target(e);
  Field *f = (Field *)lv_obj_get_user_data(o);
  if (!f || !f->fn) return;
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t c;
  lv_obj_get_coords(o, &c);
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.radius = LV_RADIUS_CIRCLE;
  const uint32_t t = millis() - f->t0;
  for (int r = 0; r < f->rows; ++r)
    for (int col = 0; col < f->cols; ++col) {
      uint32_t color = f->color;
      const float v = f->fn(f, col, r, t, &color);
      if (v < 0.03f) continue;
      d.bg_color = hexc(color);
      d.bg_opa = (lv_opa_t)(clamp01(v * f->fade) * 255);
      const int cx = c.x1 + col * f->pitch + f->pitch / 2, cy = c.y1 + r * f->pitch + f->pitch / 2;
      lv_area_t a = {(int32_t)(cx - f->radius), (int32_t)(cy - f->radius), (int32_t)(cx + f->radius - 1),
                     (int32_t)(cy + f->radius - 1)};
      lv_draw_rect(layer, &d, &a);
    }
}

void delete_cb(lv_event_t *e) {
  lv_obj_t *o = (lv_obj_t *)lv_event_get_target(e);
  delete (Field *)lv_obj_get_user_data(o);
}

// one timer drives every animated field that is visible
lv_timer_t *anim_timer;
lv_obj_t *anim_list[8];
int anim_n = 0;
void anim_tick(lv_timer_t *);

}  // namespace

lv_obj_t *field(lv_obj_t *parent, int x, int y, int cols, int rows, int pitch, int radius, DotFn fn, void *ctx,
                bool animate) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, cols * pitch, rows * pitch);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  Field *f = new Field{cols, rows, pitch, radius, fn, ctx, millis(), BLUE, animate, 1.0f};
  lv_obj_set_user_data(o, f);
  lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_add_event_cb(o, delete_cb, LV_EVENT_DELETE, nullptr);
  if (animate && anim_n < 8) anim_list[anim_n++] = o;
  if (!anim_timer) anim_timer = lv_timer_create(anim_tick, 33, nullptr);
  return o;
}
Field *field_of(lv_obj_t *o) { return (Field *)lv_obj_get_user_data(o); }
void restart(lv_obj_t *o) {
  field_of(o)->t0 = millis();
  lv_obj_invalidate(o);
}

// ---------------------------------------------------------------------------
// 5x7 font (rows top to bottom, bit 4 = leftmost column)
// ---------------------------------------------------------------------------
namespace {
struct G { char c; uint8_t r[7]; };
const G FONT[] = {
  {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
  {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
  {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
  {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
  {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
  {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
  {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
  {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
  {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
  {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
  {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
  {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
  {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
  {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
  {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
  {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
  {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
  {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
  {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},
};
}  // namespace

int glyph_w(char ch) { return ch == ':' ? 1 : ch == ' ' ? 3 : ch == ',' ? 4 : 5; }

bool glyph_dot(char ch, int x, int y) {
  if (y < 0 || y > 6 || x < 0 || x >= glyph_w(ch)) return false;
  if (ch == ':') return y == 2 || y == 4;
  if (ch >= 'a' && ch <= 'z') ch -= 32;
  for (const G &g : FONT)
    if (g.c == ch) return g.r[y] & (0x10 >> x);
  return false;
}

// ---------------------------------------------------------------------------
// text rendered into a dot bitmap
// ---------------------------------------------------------------------------
namespace {
struct DotText {
  static const int MAXC = 80, ROWS = 9;  // 1 row margin above and below the 7 rows
  uint8_t on[ROWS][MAXC];
  int cols;
  int colon_col = -1;      // so the colon can blink
  bool blink = false;
  float grid = 0.10f;      // brightness of unlit dots (the LED-panel look)
};
int layout(DotText &dt, const char *s) {
  memset(dt.on, 0, sizeof dt.on);
  int x = 1;
  dt.colon_col = -1;
  for (const char *p = s; *p && x < DotText::MAXC - 6; ++p) {
    const int w = glyph_w(*p);
    if (*p == ':') dt.colon_col = x;
    for (int gy = 0; gy < 7; ++gy)
      for (int gx = 0; gx < w; ++gx)
        if (glyph_dot(*p, gx, gy)) dt.on[gy + 1][x + gx] = 1;
    x += w + 1;
  }
  dt.cols = x;  // includes 1 col margin each side (last gap acts as the right margin)
  return dt.cols;
}
float text_fn(Field *f, int col, int row, uint32_t t, uint32_t *) {
  DotText *dt = (DotText *)f->ctx;
  if (col >= dt->cols || row >= DotText::ROWS) return 0;
  if (dt->on[row][col]) {
    if (dt->blink && col == dt->colon_col) return (millis() / 1000) % 2 ? 0.35f : 1.0f;
    return 1.0f;
  }
  return dt->grid;
}
}  // namespace

// ---------------------------------------------------------------------------
// clock
// ---------------------------------------------------------------------------
namespace {
const int CK_PITCH = 22, CK_R = 8, DT_PITCH = 9, DT_R = 3;
struct Clock { DotText time, date; lv_obj_t *ft, *fd; } ck;
}  // namespace

lv_obj_t *clock_create(lv_obj_t *parent) {
  lv_obj_t *bg = lv_obj_create(parent);
  lv_obj_remove_style_all(bg);
  lv_obj_set_size(bg, 800, 480);
  lv_obj_set_style_bg_color(bg, hexc(0x000000), 0);
  lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
  lv_obj_remove_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
  ck.time.blink = false;  // a steady colon: the clock only redraws when the minute changes
  ck.time.grid = 0.09f;
  ck.date.grid = 0.07f;
  ck.ft = field(bg, 0, 0, DotText::MAXC, DotText::ROWS, CK_PITCH, CK_R, text_fn, &ck.time, false);
  ck.fd = field(bg, 0, 0, DotText::MAXC, DotText::ROWS, DT_PITCH, DT_R, text_fn, &ck.date, false);
  return bg;
}

void clock_set(lv_obj_t *, const char *hm, const char *date, uint32_t color) {
  // Called several times a second while the clock shows: redraw only when something changed.
  static char last_hm[16] = "", last_date[32] = "";
  static uint32_t last_color = 0;
  if (!strcmp(hm, last_hm) && !strcmp(date, last_date) && color == last_color) return;
  strlcpy(last_hm, hm, sizeof last_hm);
  strlcpy(last_date, date, sizeof last_date);
  last_color = color;
  const int tc = layout(ck.time, hm), dc = layout(ck.date, date);
  Field *ft = field_of(ck.ft), *fd = field_of(ck.fd);
  ft->cols = tc;  fd->cols = dc;
  ft->color = fd->color = color;
  lv_obj_set_size(ck.ft, tc * CK_PITCH, DotText::ROWS * CK_PITCH);
  lv_obj_set_size(ck.fd, dc * DT_PITCH, DotText::ROWS * DT_PITCH);
  const int th = DotText::ROWS * CK_PITCH, dh = DotText::ROWS * DT_PITCH, gap = 18;
  const int top = (480 - (th + gap + dh)) / 2;
  lv_obj_set_pos(ck.ft, (800 - tc * CK_PITCH) / 2, top);
  lv_obj_set_pos(ck.fd, (800 - dc * DT_PITCH) / 2, top + th + gap);
  lv_obj_invalidate(ck.ft);
  lv_obj_invalidate(ck.fd);
}

// ---------------------------------------------------------------------------
// brew start: ripples, then a dot droplet, then the recipe name
// ---------------------------------------------------------------------------
namespace {
const int IN_PITCH = 20, IN_COLS = 40, IN_ROWS = 24;
const uint32_t IN_FADE = 1900, IN_END = 2600;
lv_obj_t *in_layer, *in_field, *in_name, *in_sub;
bool in_on = false;

bool in_drop(float x, float y) {  // px relative to the drop centre
  x /= 1.35f; y /= 1.35f;
  if (y < -100 || y > 92) return false;
  if (y >= 30) return x * x + (y - 30) * (y - 30) <= 62 * 62;
  const float hw = 62 * powf((y + 100) / 130.0f, 1.4f);
  return fabsf(x) <= hw;
}
float intro_fn(Field *, int col, int row, uint32_t t, uint32_t *color) {
  const float px = col * IN_PITCH + IN_PITCH / 2, py = row * IN_PITCH + IN_PITCH / 2;
  // ripples spread from where the water lands
  const float dx = px - 400, dy = (py - 250) * 1.15f, d = sqrtf(dx * dx + dy * dy);
  float v = 0;
  for (int k = 0; k < 3; ++k) {
    const float tk = (float)t - k * 260.0f;
    if (tk <= 0) continue;
    const float R = tk * 0.55f, amp = (1 - clamp01(tk / 1300.0f)) * (k == 0 ? 1.0f : 0.6f);
    const float z = (d - R) / 26.0f;
    v += amp * expf(-z * z);
  }
  // the droplet builds up dot by dot
  if (in_drop(px - 400, py - 172)) {
    const float jitter = (hash2(col, row) % 400);
    const float a = smooth(750 + jitter, 1150 + jitter, (float)t);
    // highlight on the upper left, like light on water
    const float hx = px - 400, hy = py - 172;
    const bool hl = hx < -14 && hx > -62 && hy > 0 && hy < 70;
    if (a > v) { v = a; *color = hl ? 0xD6ECFF : BLUE; }
  }
  return v;
}
void intro_click(lv_event_t *) {
  // tap: skip straight to the fade-out
  Field *f = field_of(in_field);
  const uint32_t t = millis() - f->t0;
  if (t < IN_FADE) f->t0 = millis() - IN_FADE;
}
void intro_step() {
  const uint32_t t = millis() - field_of(in_field)->t0;
  const float out = 1 - smooth(IN_FADE, IN_END, (float)t);
  lv_obj_set_style_opa(in_name, (lv_opa_t)(smooth(1100, 1500, (float)t) * out * 255), 0);
  lv_obj_set_style_opa(in_sub, (lv_opa_t)(smooth(1250, 1650, (float)t) * out * 255), 0);
  field_of(in_field)->fade = out;
  lv_obj_set_style_bg_opa(in_layer, (lv_opa_t)(out * 255), 0);
  if (t >= IN_END) {
    lv_obj_add_flag(in_layer, LV_OBJ_FLAG_HIDDEN);
    in_on = false;
  }
}

// ---------------------------------------------------------------------------
// brew finished: a cup fills, steam rises, "Enjoy" + the numbers
// ---------------------------------------------------------------------------
const int FN_PITCH = 14, FN_COLS = 21, FN_ROWS = 17;
lv_obj_t *fn_layer, *fn_field, *fn_title, *fn_name, *fn_stats[3][2];
bool fn_on = false;

uint32_t mix(uint32_t a, uint32_t b, float k);
float finish_fn(Field *, int c, int r, uint32_t t, uint32_t *color) {
  // cup outline: body cols 4..14, rows 6..14; bottom row 15; handle; saucer row 16
  const bool wall = (r >= 6 && r <= 14 && (c == 4 || c == 14)) || (r == 15 && c >= 5 && c <= 13);
  const bool handle = (c == 15 && (r == 8 || r == 11)) || (c == 16 && (r == 8 || r == 11)) ||
                      (c == 17 && r >= 9 && r <= 10);
  const bool saucer = r == 16 && c >= 2 && c <= 18;
  const float draw_in = smooth(0, 450, (float)t - (hash2(c, r) % 250));
  if (wall || handle || saucer) { *color = WHITE; return draw_in * 0.95f; }
  // coffee rising inside
  if (c >= 5 && c <= 13 && r >= 7 && r <= 14) {
    const float level = clamp01(((float)t - 350) / 900.0f) * 8;  // rows filled from the bottom
    const int from_bottom = 14 - r;
    if (from_bottom < level) {
      // brown coffee, a touch darker toward the bottom, crema on top
      *color = from_bottom + 1 > level ? 0xD9A066 : mix(0xC58850, 0x7A4B29, clamp01(1.0f - from_bottom / 7.0f));
      return from_bottom + 1 > level ? 0.45f + 0.5f * (level - from_bottom) : 0.85f;
    }
    return 0.05f;
  }
  // steam: three wavy columns that drift up and fade
  if (r <= 5 && t > 1100) {
    const float fade_in = smooth(1100, 1700, (float)t);
    for (int k = 0; k < 3; ++k) {
      const float ph = k * 2.1f;
      const int base = 6 + k * 3;
      const int x = base + (int)lroundf(0.9f * sinf(r * 0.95f - t * 0.0045f + ph));
      if (c == x) {
        const float pulse = 0.5f + 0.5f * sinf(t * 0.004f - r * 0.8f + ph);
        *color = WHITE;
        return fade_in * pulse * (0.25f + 0.13f * r);
      }
    }
  }
  return 0;
}

lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, uint32_t color) {
  lv_obj_t *l = lv_label_create(p);
  lv_label_set_text(l, "");
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, hexc(color), 0);
  return l;
}
lv_obj_t *black_layer() {
  lv_obj_t *l = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(l);
  lv_obj_set_size(l, 800, 480);
  lv_obj_set_style_bg_color(l, hexc(0x000000), 0);
  lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
  lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
  return l;
}

void build() {
  if (in_layer) return;
  in_layer = black_layer();
  in_field = field(in_layer, 0, 0, IN_COLS, IN_ROWS, IN_PITCH, 6, intro_fn, nullptr, true);
  in_name = label(in_layer, &inter_34b, 0xFFFFFF);
  in_sub = label(in_layer, &inter_20, 0x9A9AA0);
  lv_obj_add_event_cb(in_layer, intro_click, LV_EVENT_CLICKED, nullptr);

  fn_layer = black_layer();
  fn_field = field(fn_layer, (800 - FN_COLS * FN_PITCH) / 2, 34, FN_COLS, FN_ROWS, FN_PITCH, 5, finish_fn, nullptr,
                   true);
  fn_title = label(fn_layer, &inter_34b, 0xFFFFFF);
  fn_name = label(fn_layer, &inter_20, 0x9A9AA0);
  for (int i = 0; i < 3; ++i) {
    fn_stats[i][0] = label(fn_layer, &inter_34b, 0xFFFFFF);
    fn_stats[i][1] = label(fn_layer, &inter_20, 0x9A9AA0);
  }
  lv_obj_add_event_cb(fn_layer, [](lv_event_t *) { finish_hide(); }, LV_EVENT_CLICKED, nullptr);
}

// ---------------------------------------------------------------------------
// brewing scene: a dot dripper drips into a dot carafe that fills as the
// water goes in; the live numbers and the controls sit on the right.
// ---------------------------------------------------------------------------
const int BS_PITCH = 15, BS_COLS = 26, BS_ROWS = 29;
lv_obj_t *bs_layer, *bs_field, *bs_name, *bs_title, *bs_sub, *bs_water, *bs_water_u, *bs_stat[2][2], *bs_cap[2],
    *bs_pause, *bs_pause_lbl, *bs_pause_icon, *bs_stop;
bool bs_on = false;
BrewView bs_v = {};
float bs_level = 0;          // shown level 0..1, eases toward the real one
uint32_t bs_last_ms = 0;
void (*bs_on_pause)() = nullptr;
void (*bs_on_stop)() = nullptr;

uint32_t mix(uint32_t a, uint32_t b, float k) {
  const int ra = a >> 16, ga = (a >> 8) & 255, ba = a & 255, rb = b >> 16, gb = (b >> 8) & 255, bb = b & 255;
  return ((uint32_t)(ra + (rb - ra) * k) << 16) | ((uint32_t)(ga + (gb - ga) * k) << 8) | (uint32_t)(ba + (bb - ba) * k);
}

float brew_fn(Field *, int c, int r, uint32_t t, uint32_t *color) {
  const float appear = smooth(0, 500, (float)t - (hash2(c, r) % 300));
  // dripper: a cone, rows 1..8, centred on col 12.5
  if (r >= 1 && r <= 8) {
    const float half = 6.5f - (r - 1) * 0.72f;
    const float dx = fabsf(c - 12.5f);
    if (r == 1 && dx <= half) { *color = WHITE; return appear * 0.9f; }                  // rim
    if (fabsf(dx - half) < 0.5f) { *color = WHITE; return appear * 0.9f; }              // sides
    if (dx < half - 0.5f && r >= 3) {                                                 // the wet coffee bed
      *color = mix(0x5A3A20, COFFEE, 0.4f + 0.3f * sinf(c * 1.7f + r));
      return appear * (bs_v.pouring ? 0.85f : 0.6f);
    }
    // hot water falling into the dripper from above while pouring
    return 0;
  }
  // the drip from the tip (col 12/13) down to the coffee surface
  const int top_row = 13, bottom_row = 27, rows_in = bottom_row - top_row;  // carafe inner rows 14..26
  const int surface = bottom_row - (int)lroundf(bs_level * (rows_in - 1));
  if ((c == 12 || c == 13) && r >= 9 && r < surface) {
    const float speed = 0.012f;                                     // rows per ms
    if (bs_v.pouring && !bs_v.paused) {
      // a steady stream, dots running down
      const float phase = fmodf(r - t * speed, 3.0f);
      *color = mix(COFFEE, 0xD9A066, 0.3f);
      return c == 12 ? 0.55f + 0.4f * (phase < 1.2f) : 0.0f;
    }
    // between pours: a single drop now and then
    const float cyc = fmodf(t * speed * 0.5f, 30.0f);
    if (c == 12 && fabsf(r - (9 + cyc)) < 0.6f && !bs_v.paused) { *color = COFFEE; return 0.8f; }
    return 0;
  }
  // carafe: walls cols 4 and 21, rows 12..27, rounded bottom corners
  const bool wall = ((c == 4 || c == 21) && r >= 12 && r <= 26) || (r == 28 && c >= 6 && c <= 19) ||
                    (r == 27 && (c == 5 || c == 20));
  const bool spout = r == 12 && (c == 3 || c == 22);
  if (wall || spout) { *color = WHITE; return appear * 0.85f; }
  if (c >= 5 && c <= 20 && r >= 14 && r <= 27 && !(r == 27 && (c == 5 || c == 20))) {
    if (r > surface) {
      // coffee: darker toward the bottom, a little sparkle
      const float depth = (float)(r - surface) / rows_in;
      *color = mix(0xB97A45, 0x6E4426, clamp01(depth * 1.6f));
      return appear * (0.8f + 0.12f * sinf(t * 0.003f + c * 0.9f + r * 1.3f));
    }
    if (r == surface && bs_level > 0.01f) {
      // the surface: crema-coloured, rippling where the drip lands
      const float d = fabsf(c - 12.5f);
      const float wave = 0.5f + 0.5f * sinf(d * 1.3f - t * 0.008f);
      *color = mix(COFFEE, 0xE0AE74, 0.6f);
      return appear * (bs_v.pouring ? 0.55f + 0.4f * wave : 0.75f);
    }
    return 0.05f;  // empty glass: barely there
  }
  return 0;
}

lv_obj_t *brew_btn(lv_obj_t *p, int x, int y, int w, int h, uint32_t bg, lv_obj_t **icon, lv_obj_t **lbl,
                   const char *sym, const char *txt, uint32_t fg, void (*cb)(lv_event_t *)) {
  lv_obj_t *b = lv_obj_create(p);
  lv_obj_remove_style_all(b);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_radius(b, h / 2, 0);
  lv_obj_set_style_bg_color(b, hexc(bg), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(b, hexc(0xF2F2F2), 0);
  lv_obj_set_style_border_width(b, bg == 0x000000 ? 3 : 0, 0);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(b, 10, 0);
  lv_obj_t *ic = lv_label_create(b);  // symbols come from the Montserrat icon font
  lv_label_set_text(ic, sym);
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(ic, hexc(fg), 0);
  if (icon) *icon = ic;
  if (txt) {
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &inter_20b, 0);
    lv_obj_set_style_text_color(l, hexc(fg), 0);
    if (lbl) *lbl = l;
  }
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

void build_brew() {
  if (bs_layer) return;
  bs_layer = black_layer();
  bs_field = field(bs_layer, 24, 26, BS_COLS, BS_ROWS, BS_PITCH, 5, brew_fn, nullptr, true);
  const int X = 440;
  bs_name = label(bs_layer, &inter_20, 0x9A9AA0);
  lv_obj_set_pos(bs_name, X, 34);
  bs_title = label(bs_layer, &inter_34b, 0xFFFFFF);
  lv_obj_set_pos(bs_title, X, 64);
  bs_sub = label(bs_layer, &inter_20, 0xC8925A);
  lv_obj_set_pos(bs_sub, X, 110);
  bs_water = label(bs_layer, &inter_64b, 0xFFFFFF);
  bs_water_u = label(bs_layer, &inter_20, 0x9A9AA0);
  const char *caps[2] = {"Scale", "Time"};
  for (int i = 0; i < 2; ++i) {
    bs_cap[i] = label(bs_layer, &inter_16, 0x8E8E93);
    lv_label_set_text(bs_cap[i], caps[i]);
    lv_obj_set_pos(bs_cap[i], X + i * 170, 262);
    bs_stat[i][0] = label(bs_layer, &inter_34b, 0xFFFFFF);
    bs_stat[i][1] = label(bs_layer, &inter_16, 0x9A9AA0);
  }
  bs_pause = brew_btn(bs_layer, X, 370, 230, 70, 0xF2F2F2, &bs_pause_icon, &bs_pause_lbl, LV_SYMBOL_PAUSE, "PAUSE",
                      0x000000, [](lv_event_t *) { if (bs_on_pause) bs_on_pause(); });
  bs_stop = brew_btn(bs_layer, X + 246, 370, 70, 70, 0x000000, nullptr, nullptr, LV_SYMBOL_STOP, nullptr, 0xF2F2F2,
                     [](lv_event_t *) { if (bs_on_stop) bs_on_stop(); });
}

// number + small unit on a shared baseline
void place_pair(lv_obj_t *n, lv_obj_t *u, int x, int base) {
  lv_obj_update_layout(n);
  const lv_font_t *fn = lv_obj_get_style_text_font(n, 0), *fu = lv_obj_get_style_text_font(u, 0);
  lv_obj_set_pos(n, x, base - (lv_font_get_line_height(fn) - fn->base_line));
  lv_obj_set_pos(u, x + lv_obj_get_width(n) + 6, base - (lv_font_get_line_height(fu) - fu->base_line));
}
void anim_tick(lv_timer_t *) {
  for (int i = 0; i < anim_n; ++i) {
    lv_obj_t *o = anim_list[i];
    lv_obj_t *top = lv_obj_get_parent(o);
    if (!lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) && !(top && lv_obj_has_flag(top, LV_OBJ_FLAG_HIDDEN)))
      lv_obj_invalidate(o);
  }
  if (in_on) intro_step();
  if (bs_on) {
    const float target = bs_v.total_ml > 0 ? clamp01(bs_v.water_ml / bs_v.total_ml) : 0;
    bs_level += (target - bs_level) * 0.08f;
  }
  if (fn_on) {
    const float t = (float)(millis() - field_of(fn_field)->t0);
    lv_obj_set_style_opa(fn_title, (lv_opa_t)(smooth(900, 1300, t) * 255), 0);
    lv_obj_set_style_opa(fn_name, (lv_opa_t)(smooth(1000, 1400, t) * 255), 0);
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 2; ++j)
        lv_obj_set_style_opa(fn_stats[i][j], (lv_opa_t)(smooth(1150 + i * 120, 1550 + i * 120, t) * 255), 0);
  }
}

}  // namespace

void intro_play(const char *name, const char *sub) {
  build();
  lv_label_set_text(in_name, name);
  lv_label_set_text(in_sub, sub);
  lv_obj_align(in_name, LV_ALIGN_TOP_MID, 0, 318);
  lv_obj_align(in_sub, LV_ALIGN_TOP_MID, 0, 368);
  lv_obj_set_style_bg_opa(in_layer, LV_OPA_COVER, 0);
  field_of(in_field)->fade = 1;
  lv_obj_set_style_opa(in_name, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(in_sub, LV_OPA_TRANSP, 0);
  lv_obj_remove_flag(in_layer, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(in_layer);
  restart(in_field);
  in_on = true;
}

void finish_play(const char *name, float grams, float ml, uint32_t ms) {
  build();
  in_on = false;
  lv_obj_add_flag(in_layer, LV_OBJ_FLAG_HIDDEN);
  brew_hide();
  lv_label_set_text(fn_title, "Enjoy");
  lv_label_set_text(fn_name, name);
  lv_obj_align(fn_title, LV_ALIGN_TOP_MID, 0, 290);
  lv_obj_align(fn_name, LV_ALIGN_TOP_MID, 0, 336);
  char v[3][16];
  snprintf(v[0], 16, "%.1f", grams);
  snprintf(v[1], 16, "%.0f", ml);
  snprintf(v[2], 16, "%lu:%02lu", (unsigned long)(ms / 60000), (unsigned long)(ms / 1000 % 60));
  const char *u[3] = {"g", "ml", "min"};
  for (int i = 0; i < 3; ++i) {
    lv_label_set_text(fn_stats[i][0], v[i]);
    lv_label_set_text(fn_stats[i][1], u[i]);
    // number + unit, centred as a pair in each third, on a shared baseline
    lv_obj_update_layout(fn_stats[i][0]);
    lv_obj_update_layout(fn_stats[i][1]);
    const int wn = lv_obj_get_width(fn_stats[i][0]), wu = lv_obj_get_width(fn_stats[i][1]);
    const int cx = 160 + i * 240, x0 = cx - (wn + 6 + wu) / 2, base = 418;
    const lv_font_t *fb = &inter_34b, *fs = &inter_20;
    lv_obj_set_pos(fn_stats[i][0], x0, base - (lv_font_get_line_height(fb) - fb->base_line));
    lv_obj_set_pos(fn_stats[i][1], x0 + wn + 6, base - (lv_font_get_line_height(fs) - fs->base_line));
  }
  lv_obj_remove_flag(fn_layer, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(fn_layer);
  restart(fn_field);
  fn_on = true;
}

void brew_show(const char *name, void (*on_pause)(), void (*on_stop)()) {
  build();
  build_brew();
  bs_on_pause = on_pause;
  bs_on_stop = on_stop;
  bs_level = 0;
  bs_v = {};
  lv_label_set_text(bs_name, name);
  lv_obj_remove_flag(bs_layer, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(bs_layer);
  restart(bs_field);
  bs_on = true;
}

void brew_update(const BrewView &v) {
  if (!bs_on) return;
  bs_v = v;
  lv_label_set_text(bs_title, v.paused ? "Paused" : v.pour_title);
  lv_label_set_text(bs_sub, v.pour_sub);
  char b[24];
  snprintf(b, sizeof b, "%.0f", v.water_ml);
  lv_label_set_text(bs_water, b);
  snprintf(b, sizeof b, "/ %.0f ml", v.total_ml);
  lv_label_set_text(bs_water_u, b);
  place_pair(bs_water, bs_water_u, 440, 236);
  snprintf(b, sizeof b, "%.1f", v.grams);
  lv_label_set_text(bs_stat[0][0], b);
  lv_label_set_text(bs_stat[0][1], "g");
  snprintf(b, sizeof b, "%lu:%02lu", (unsigned long)(v.ms / 60000), (unsigned long)(v.ms / 1000 % 60));
  lv_label_set_text(bs_stat[1][0], b);
  lv_label_set_text(bs_stat[1][1], "min");
  for (int i = 0; i < 2; ++i) place_pair(bs_stat[i][0], bs_stat[i][1], 440 + i * 170, 326);
  lv_label_set_text(bs_pause_lbl, v.paused ? "RESUME" : "PAUSE");
  lv_label_set_text(bs_pause_icon, v.paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
}

void brew_hide() {
  if (bs_layer) lv_obj_add_flag(bs_layer, LV_OBJ_FLAG_HIDDEN);
  bs_on = false;
}
bool brew_showing() { return bs_on; }

void finish_hide() {
  if (fn_layer) lv_obj_add_flag(fn_layer, LV_OBJ_FLAG_HIDDEN);
  fn_on = false;
}
bool finish_showing() { return fn_on; }
bool playing() { return in_on; }

}  // namespace dotfx
