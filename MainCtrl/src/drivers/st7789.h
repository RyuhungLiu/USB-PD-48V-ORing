#pragma once

#include <stdint.h>

// The panel is natively 240x320 portrait; fixed here to 320x240 landscape
#define LCD_WIDTH   320
#define LCD_HEIGHT  240

// MADCTL: 0x60 = MX|MV landscape; if the image is upside down and mirrored, change to 0xA0 (MY|MV)
// Set 0x08 (BGR) if red and blue are swapped
#ifndef LCD_MADCTL
#define LCD_MADCTL  0x60
#endif

// IPS panels usually need inversion (INVON); change to 0 if colors look like a negative
#ifndef LCD_INVERT
#define LCD_INVERT  1
#endif

// Frame memory offset (generally 0 for 240x320 panels)
#define LCD_X_OFFSET 0
#define LCD_Y_OFFSET 0

void lcd_init(void);

// The window must be set before drawing; coordinates include the end points
void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

// Blocking: returns after DMA completes and SPI is idle
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void lcd_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels);
