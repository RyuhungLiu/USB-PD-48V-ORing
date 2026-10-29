#include "widget.h"

#include <string.h>
#include "ui/gfx.h"

void label_invalidate(label_t *label)
{
    label->drawn = false;
}

void label_set(label_t *label, const char *text, uint16_t fg)
{
    uint8_t width = label->width;
    if (width > LABEL_MAX_CHARS) width = LABEL_MAX_CHARS;

    char padded[LABEL_MAX_CHARS + 1];
    size_t len = strlen(text);
    if (len > width) len = width;

    uint8_t pad = width - len;
    if (label->right_align)
    {
        memset(padded, ' ', pad);
        memcpy(padded + pad, text, len);
    }
    else
    {
        memcpy(padded, text, len);
        memset(padded + len, ' ', pad);
    }
    padded[width] = '\0';

    if (label->drawn && label->fg == fg && strcmp(label->last, padded) == 0) return;

    // Redraw only the characters that changed
    uint16_t x = label->x;
    for (uint8_t i = 0; i < width; i++)
    {
        if (!label->drawn || label->fg != fg || label->last[i] != padded[i])
            gfx_draw_char(x, label->y, label->font, padded[i], fg, label->bg);
        x += label->font->width;
    }

    memcpy(label->last, padded, width + 1);
    label->fg = fg;
    label->drawn = true;
}
