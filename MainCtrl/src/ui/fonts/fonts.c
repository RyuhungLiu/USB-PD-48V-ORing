#include "fonts.h"

static uint16_t glyph_bytes(const font_t *font)
{
    uint8_t bpp = font->bpp ? font->bpp : 1;
    return (uint16_t)((font->width * bpp + 7) / 8) * font->height;
}

int font_index(const font_t *font, char ch)
{
    uint8_t c = (uint8_t)ch;

    if (font->map == NULL)
    {
        if (c < font->first || c >= font->first + font->count) return -1;
        return c - font->first;
    }

    for (uint8_t i = 0; i < font->count; i++)
    {
        if ((uint8_t)font->map[i] == c) return i;
    }
    return -1;
}

const uint8_t *font_glyph(const font_t *font, char ch)
{
    int i = font_index(font, ch);
    if (i < 0) return NULL;
    return font->bitmap + (font->offset ? font->offset[i] : (uint32_t)i * glyph_bytes(font));
}

uint8_t font_advance(const font_t *font, char ch)
{
    int i = font_index(font, ch);
    return (font->advance && i >= 0) ? font->advance[i] : font->width;
}
