#include "gfx.h"

#include <string.h>

// Glyph expansion buffer: sized for the largest font actually used, 16x24 -> 768B (large digits are drawn with geometric strokes in ui/seg.c instead)
#define GLYPH_BUF_PIXELS (16 * 24)
#define GLYPH_ROW_MAX    12          // maximum bytes per row in compressed fonts (home page total output 31px 2bpp = 8)
static uint16_t glyph_buf[GLYPH_BUF_PIXELS];

void gfx_fill_screen(uint16_t color)
{
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, color);
}

void gfx_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    lcd_fill_rect(x, y, w, h, color);
}

void gfx_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color)
{
    lcd_fill_rect(x, y, w, 1, color);
}

void gfx_vline(uint16_t x, uint16_t y, uint16_t h, uint16_t color)
{
    lcd_fill_rect(x, y, 1, h, color);
}

void gfx_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (w == 0 || h == 0) return;

    gfx_hline(x, y, w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x + w - 1, y, h, color);
}

// Integer square root (for computing circle half-widths)
static uint16_t isqrt(uint32_t v)
{
    uint32_t r = 0, bit = 1UL << 30;
    while (bit > v) bit >>= 2;
    while (bit)
    {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint16_t)r;
}

void gfx_fill_circle(uint16_t cx, uint16_t cy, uint16_t r, uint16_t color)
{
    for (int16_t dy = -(int16_t)r; dy <= (int16_t)r; dy++)
    {
        uint16_t half = isqrt((uint32_t)r * r - (uint32_t)(dy * dy));
        lcd_fill_rect(cx - half, cy + dy, half * 2 + 1, 1, color);
    }
}

void gfx_ring(uint16_t cx, uint16_t cy, uint16_t r, uint16_t thick, uint16_t fg, uint16_t bg)
{
    gfx_fill_circle(cx, cy, r, fg);
    if (thick < r) gfx_fill_circle(cx, cy, r - thick, bg);
}

void gfx_fill_triangle(uint16_t x, uint16_t y, uint16_t size, uint16_t color)
{
    for (uint16_t row = 0; row < size; row++)
    {
        uint16_t half = (uint16_t)((uint32_t)row * size / (2 * size));     // row width grows linearly with the row
        lcd_fill_rect(x + size / 2 - half, y + row, half * 2 + 1, 1, color);
    }
}

// RGB565 linear blend: k = 0..n maps to a..b
uint16_t gfx_mix565(uint16_t a, uint16_t b, uint8_t k, uint8_t n)
{
    if (k >= n) return b;
    uint16_t r = (uint16_t)((((a >> 11) & 0x1F) * (n - k) + ((b >> 11) & 0x1F) * k) / n);
    uint16_t g = (uint16_t)((((a >> 5) & 0x3F) * (n - k) + ((b >> 5) & 0x3F) * k) / n);
    uint16_t c = (uint16_t)(((a & 0x1F) * (n - k) + (b & 0x1F) * k) / n);
    return (uint16_t)((r << 11) | (g << 5) | c);
}

// 4 gray levels of 2bpp anti-aliasing: k = 0..3 maps to bg..fg
#define blend565(bg, fg, k) gfx_mix565((bg), (fg), (k), 3)

// Large glyphs are drawn in strips: at most GLYPH_BUF_PIXELS pixels per strip, no whole-glyph buffer needed
uint16_t gfx_draw_char(uint16_t x, uint16_t y, const font_t *font, char ch, uint16_t fg, uint16_t bg)
{
    uint16_t w = font_advance(font, ch);
    uint16_t h = font->height;
    if (w == 0 || w > GLYPH_BUF_PIXELS) return x;
    if (x + w > LCD_WIDTH || y + h > LCD_HEIGHT) return x;

    const uint8_t *glyph = font_glyph(font, ch);
    if (glyph == NULL)
    {
        lcd_fill_rect(x, y, w, h, bg);
        return x + w;
    }

    uint8_t bpp = font->bpp ? font->bpp : 1;
    uint16_t row_bytes = (uint16_t)((font->width * bpp + 7) / 8);
    uint16_t lut[4] = {bg, fg, fg, fg};
    if (bpp == 2)
    {
        lut[1] = blend565(bg, fg, 1);
        lut[2] = blend565(bg, fg, 2);
    }

    // Compressed fonts are decoded row by row into rowb: 0x00 followed by the count of consecutive 0 bytes (including this one)
    uint8_t rowb[GLYPH_ROW_MAX];
    uint8_t run = 0;
    if (font->offset && row_bytes > sizeof(rowb)) return x;

    uint16_t rows_per_strip = GLYPH_BUF_PIXELS / w;
    for (uint16_t row0 = 0; row0 < h; row0 += rows_per_strip)
    {
        uint16_t rows = (h - row0 < rows_per_strip) ? (h - row0) : rows_per_strip;
        uint16_t *p = glyph_buf;
        for (uint16_t row = row0; row < row0 + rows; row++)
        {
            const uint8_t *src = glyph + row * row_bytes;
            if (font->offset)
            {
                for (uint16_t i = 0; i < row_bytes; i++)
                {
                    uint8_t b = 0;
                    if (run) run--;
                    else if ((b = *glyph++) == 0) run = (uint8_t)(*glyph++ - 1);
                    rowb[i] = b;
                }
                src = rowb;
            }
            for (uint16_t col = 0; col < w; col++)
            {
                if (bpp == 2)
                    *p++ = lut[(src[col >> 2] >> (6 - 2 * (col & 3))) & 3];
                else
                    *p++ = (src[col >> 3] & (0x80 >> (col & 7))) ? fg : bg;
            }
        }
        lcd_blit(x, y + row0, w, rows, glyph_buf);
    }
    return x + w;
}

uint16_t gfx_draw_string(uint16_t x, uint16_t y, const font_t *font, const char *str, uint16_t fg, uint16_t bg)
{
    while (*str)
    {
        if (x + font_advance(font, *str) > LCD_WIDTH) break;
        x = gfx_draw_char(x, y, font, *str++, fg, bg);
    }
    return x;
}

uint16_t gfx_string_width(const font_t *font, const char *str)
{
    if (!font->advance) return (uint16_t)(strlen(str) * font->width);
    uint16_t w = 0;
    while (*str) w += font_advance(font, *str++);
    return w;
}

uint16_t gfx_draw_string_right(uint16_t x_right, uint16_t y, uint16_t box_w, const font_t *font,
                               const char *str, uint16_t fg, uint16_t bg)
{
    uint16_t w = gfx_string_width(font, str);
    if (w > box_w) w = box_w;
    uint16_t x0 = x_right - box_w;
    if (box_w > w) lcd_fill_rect(x0, y, box_w - w, font->height, bg);
    return gfx_draw_string(x_right - w, y, font, str, fg, bg);
}
