#pragma once

#include <stdint.h>
#include "drivers/st7789.h"
#include "ui/fonts/fonts.h"

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// EVA Unit-01 color scheme: purple as the structural color, EVA green as the single main accent, NERV orange for warnings;
// text uses a purple-tinted light white that does not compete with the accent (ST7789 renders darker, so high luminance values are used)
#define BH_BG           RGB565(0, 0, 0)        // pure black; TFT backlight leakage still shows dark gray
#define BH_INK          RGB565(236, 230, 255)  // primary values: pale lavender white
#define BH_DIM          RGB565(150, 128, 200)  // labels: grayish purple
#define BH_GRID         RGB565(72, 42, 112)    // grid lines, separators: dark purple
#define BH_RED          RGB565(255, 60, 92)    // alarm / current too high: magenta red
#define BH_BLUE         RGB565(160, 96, 255)   // Unit-01 purple: current too low, page entries
#define BH_ACCENT       RGB565(118, 226, 48)   // EVA green: total output block, title marker, on state
#define BH_YELLOW       RGB565(255, 150, 0)    // NERV orange: waiting / hints

// Semantic color names used by the settings pages, all mapped onto the EVA palette:
//   on / normal (formerly green) uses EVA green, page entries (formerly cyan) use purple, hints use amber, non-latched trips use orange, errors use red;
//   cursor row (formerly dark blue) uses dark purple
#define COLOR_BLACK     BH_BG
#define COLOR_WHITE     BH_INK
#define COLOR_GRAY      BH_DIM
#define COLOR_DARKGRAY  BH_GRID
#define COLOR_RED       BH_RED
#define COLOR_BLUE      BH_BLUE
#define COLOR_GREEN     BH_ACCENT
#define COLOR_CYAN      BH_BLUE
#define COLOR_YELLOW    BH_YELLOW
#define COLOR_ORANGE    RGB565(255, 96, 24)
#define COLOR_NAVY      RGB565(46, 22, 78)      // cursor row background (dark purple)

void gfx_fill_screen(uint16_t color);
void gfx_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

// RGB565 linear blend: k = 0..n maps to a..b
uint16_t gfx_mix565(uint16_t a, uint16_t b, uint8_t k, uint8_t n);
void gfx_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void gfx_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color);
void gfx_vline(uint16_t x, uint16_t y, uint16_t h, uint16_t color);

// Filled circle (center cx,cy, radius r); ring = outer circle in foreground + inner circle in background
void gfx_fill_circle(uint16_t cx, uint16_t cy, uint16_t r, uint16_t color);
void gfx_ring(uint16_t cx, uint16_t cy, uint16_t r, uint16_t thick, uint16_t fg, uint16_t bg);
// Filled equilateral triangle (pointing up), bounding box top-left x,y, side length size
void gfx_fill_triangle(uint16_t x, uint16_t y, uint16_t size, uint16_t color);

// Characters not in the font are drawn as background-colored spaces; returns the x coordinate after drawing
uint16_t gfx_draw_char(uint16_t x, uint16_t y, const font_t *font, char ch, uint16_t fg, uint16_t bg);
uint16_t gfx_draw_string(uint16_t x, uint16_t y, const font_t *font, const char *str, uint16_t fg, uint16_t bg);
uint16_t gfx_string_width(const font_t *font, const char *str);

// Right-aligned drawing into a region of width box_w with right edge x_right; the blank on the left is filled with background (erases leftovers when the value gets shorter)
uint16_t gfx_draw_string_right(uint16_t x_right, uint16_t y, uint16_t box_w, const font_t *font,
                               const char *str, uint16_t fg, uint16_t bg);
