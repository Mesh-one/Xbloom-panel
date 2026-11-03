// Colours, fonts and spacing, modelled on the xBloom app's dark theme.
#pragma once
#include <lvgl.h>

LV_FONT_DECLARE(inter_14);
LV_FONT_DECLARE(inter_16);
LV_FONT_DECLARE(inter_20);
LV_FONT_DECLARE(inter_20b);
LV_FONT_DECLARE(inter_26b);
LV_FONT_DECLARE(inter_34b);
LV_FONT_DECLARE(inter_64b);
LV_FONT_DECLARE(inter_96_num);
LV_FONT_DECLARE(inter_120_bold);
LV_FONT_DECLARE(inter_clock);
LV_FONT_DECLARE(inter_44);

namespace th {
inline lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

// ---- spacing (8-point grid) -------------------------------------------------
const int SB = 36;       // status bar height
const int M = 24;        // screen margin
const int G = 16;        // gap between blocks
const int R = 20;        // card corner radius
const int R_SM = 14;     // small card / button radius
const int W = 800, H = 480;
const int TABBAR_H = 56;
const int MOD_TOP = 152; // where controls start on Grinder / Brewer / Scale
const int MOD_BOTTOM = H - M;
const int MOD_LEFT_W = 540;                 // left column on module screens
const int MOD_RIGHT_X = M + MOD_LEFT_W + 20;
const int MOD_RIGHT_W = W - M - MOD_RIGHT_X;

// ---- colours ----------------------------------------------------------------
const uint32_t BG = 0x000000;
const uint32_t BG_MOD = 0x111111;  // Grinder / Brewer / Scale / recipe screens
const uint32_t TABBAR = 0x1C1C1E;
const uint32_t HAIRLINE = 0x2C2C2E;
const uint32_t SEG_BG = 0x2C2C2E;
const uint32_t SURFACE = 0x222222;  // unselected option buttons
const uint32_t TEXT = 0xFFFFFF;
const uint32_t MUTED = 0x8E8E93;
const uint32_t SUB = 0xAEAEB2;
const uint32_t GREEN = 0x34C759;

// module tiles: background, accent (label + dots)
const uint32_t GRINDER_BG = 0x2A1E16, GRINDER_FG = 0xC4976E, GRINDER_FILL = 0x382A20;
const uint32_t BREWER_BG = 0x161B2A, BREWER_FG = 0x7887C8, BREWER_FILL = 0x20263A, BREWER_SEL = 0x222A3D;
const uint32_t SCALE_BG = 0x1A2622, SCALE_FG = 0x7CC4AE, SCALE_FILL = 0x1D2624, SCALE_SEL = 0x1B2826;
const uint32_t FAV_PANEL = 0x4A413B;

// pastel recipe cards
const uint32_t CARDS[] = {0xE2DBA6, 0xC8D6B5, 0xE4A3A4, 0xB0BDE0, 0xE6C3A0, 0xC9B8DE};
const int N_CARDS = sizeof(CARDS) / sizeof(CARDS[0]);
}  // namespace th
