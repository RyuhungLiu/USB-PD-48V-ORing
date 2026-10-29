#pragma once

#include <stddef.h>
#include <stdint.h>

// Bitmap fonts: each glyph has height rows
//   bpp 0/1 : 1bpp, (width+7)/8 bytes per row, MSB on the left
//   bpp 2   : 2bpp anti-aliased (4 gray levels), (width+3)/4 bytes per row, high bits on the left
// map == NULL : characters are a contiguous range starting at first
// map != NULL : characters are the sparse set listed in the map string (first is unused)
// advance != NULL : proportional width, each glyph draws only advance[i] columns (the glyph cell is still width)
// offset != NULL  : bitmap is per-glyph zero run-length compressed (0x00 followed by the count of consecutive 0 bytes), offset[i] is the start of glyph i in bitmap
typedef struct font_t
{
    uint8_t width;
    uint8_t height;
    uint8_t first;
    uint8_t count;
    const char *map;
    const uint8_t *bitmap;
    uint8_t bpp;
    const uint8_t *advance;
    const uint16_t *offset;
} font_t;

extern const font_t font_8x16;
extern const font_t font_16x24;   // only " %()*+-./0-9>A-Z[]", other characters are drawn blank
extern const font_t font_num_ch;  // Chakra Petch Bold 2bpp, "0123456789.", tabular digits (home page channel power)
extern const font_t font_num_out; // Chakra Petch Bold 2bpp, "0123456789.", tabular digits (home page total output)

// Glyph index; returns -1 if not in the font
int font_index(const font_t *font, char ch);

// Returns a pointer to the glyph data (for compressed fonts, the start of that glyph's compressed data); returns NULL for characters not in the font
const uint8_t *font_glyph(const font_t *font, char ch);

// Character advance width (width for missing characters)
uint8_t font_advance(const font_t *font, char ch);
