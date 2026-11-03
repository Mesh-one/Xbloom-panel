// Dot-matrix effects in the style of the xBloom's blue LED display:
// a dot clock, the brew-start animation and the brew-finished animation.
#pragma once
#include <lvgl.h>
#include <stdint.h>

namespace dotfx {

// Colours
extern const uint32_t BLUE;   // xBloom LED blue
extern const uint32_t WHITE;

// A field of round dots drawn every frame by `fn`, which returns the dot's
// brightness 0..1 (and may pick its colour). `t` = ms since the field started.
struct Field;
typedef float (*DotFn)(Field *f, int col, int row, uint32_t t, uint32_t *color);
struct Field {
  int cols, rows, pitch, radius;
  DotFn fn;
  void *ctx;
  uint32_t t0;
  uint32_t color;
  bool animate;     // redraw every frame (false: only when told)
  float fade;       // multiplies every dot's brightness
};
lv_obj_t *field(lv_obj_t *parent, int x, int y, int cols, int rows, int pitch, int radius, DotFn fn, void *ctx,
                bool animate);
Field *field_of(lv_obj_t *o);
void restart(lv_obj_t *o);

// 5x7 dot font: true if the dot at (x,y) of glyph `ch` is lit (x 0..4, y 0..6). ':' is 1 wide.
bool glyph_dot(char ch, int x, int y);
int glyph_w(char ch);

// ---- dot clock ----
lv_obj_t *clock_create(lv_obj_t *parent);           // full-screen, black
void clock_set(lv_obj_t *clk, const char *hm, const char *date, uint32_t color);

// ---- brewing scene: shown for the whole brew ----
struct BrewView {
  float water_ml, total_ml, grams;
  uint32_t ms;
  const char *pour_title;   // "Pour 2 of 3"
  const char *pour_sub;     // "85°C  |  Spiral"
  bool pouring;             // water is flowing right now
  bool paused;
};
void brew_show(const char *recipe_name, void (*on_pause)(), void (*on_stop)());
void brew_update(const BrewView &v);
void brew_hide();
bool brew_showing();
extern const uint32_t COFFEE;

// ---- brew start / finish ----
void intro_play(const char *recipe_name, const char *sub);          // ~2.6 s, tap to skip
void finish_play(const char *recipe_name, float grams, float ml, uint32_t ms);
void finish_hide();
bool finish_showing();
bool playing();

}  // namespace dotfx
