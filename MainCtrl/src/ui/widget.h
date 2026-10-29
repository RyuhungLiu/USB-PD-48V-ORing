#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ui/fonts/fonts.h"

#define LABEL_MAX_CHARS 40      // 8x16 font, full row 320px

// Text widget: redrawn only when content or color changes; fixed width of width characters, right- or left-aligned
typedef struct label_t
{
    uint16_t x;
    uint16_t y;
    const font_t *font;
    uint8_t width;          // character count; shorter text is padded with spaces to erase leftovers
    bool right_align;
    uint16_t bg;

    // Runtime cache
    uint16_t fg;
    bool drawn;
    char last[LABEL_MAX_CHARS + 1];
} label_t;

// Force a redraw on the next label_set (call after a page change)
void label_invalidate(label_t *label);

void label_set(label_t *label, const char *text, uint16_t fg);
