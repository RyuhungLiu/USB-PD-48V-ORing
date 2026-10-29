#include "page_trend.h"

#include <stdio.h>
#include <string.h>
#include "app/trend.h"
#include "ui/fmt.h"
#include "ui/gfx.h"
#include "ui/widget.h"

// Layout (320x240)
//  y=0    title bar "TREND 30 S" / "TREND 5 MIN"
//  y=24   legend CH1 (white) CH2 (blue) OUT (yellow)         right side: full scale and total power peak within the window
//  y=42   plot area 300x172 (x=10..309, grid ticks drawn in the side margins): 4 horizontal dotted grid divisions; vertical grid lines every 5 seconds in the 30 s range, every 1 minute in the 5 min range
//  y=218  time axis: leftmost is the window start, rightmost is NOW
//
// No full-screen buffer: each column is composed in RAM (background + grid + vertical spans of the three curves) and sent by DMA,
// each UI refresh draws only COLS_PER_FRAME columns, so the whole plot completes over several frames without hogging the main loop

#define PLOT_X          10              // 10px margin on each side, symmetric and centered
#define PLOT_Y          42
#define PLOT_W          TREND_LEN
#define PLOT_H          172
#define COLS_PER_FRAME  60

static trend_scale_t scale = TREND_FINE;
static uint16_t col[PLOT_H];
static uint16_t render_x = PLOT_W;      // next column; PLOT_W = finished drawing
static uint32_t rendered_seq;
static int32_t  full_scale;             // full scale, 0.1W
static int32_t  peak;                   // total power peak within the window, 0.1W
static label_t  range_lb, axis_lb;

// Full scale takes a "nice" step (W)
static int32_t nice_scale(int32_t max_d10)
{
    static const int16_t steps_w[] = {5, 10, 20, 50, 100, 200, 300, 500};
    for (uint8_t i = 0; i < sizeof(steps_w) / sizeof(steps_w[0]); i++)
        if (max_d10 <= steps_w[i] * 10) return steps_w[i] * 10;
    return 5000;
}

static void restart(void)
{
    uint16_t n = trend_count(scale);
    peak = 0;
    for (uint16_t i = 0; i < n; i++)
    {
        trend_pt_t p = trend_get(scale, i);
        if (p.p1 + p.p2 > peak) peak = p.p1 + p.p2;
    }
    full_scale = nice_scale(peak);
    rendered_seq = trend_seq(scale);
    render_x = 0;
}

static uint16_t map_y(int32_t v)
{
    if (v < 0) v = 0;
    if (v > full_scale) v = full_scale;
    return (uint16_t)(PLOT_H - 1 - v * (PLOT_H - 1) / full_scale);
}

// Vertical span (including 1 pixel of thickening), guarantees continuity between adjacent points
static void span(int32_t v_prev, int32_t v, uint16_t color)
{
    uint16_t a = map_y(v_prev), b = map_y(v);
    if (a > b) { uint16_t t = a; a = b; b = t; }
    if (a > 0) a--;
    for (uint16_t y = a; y <= b; y++) col[y] = color;
}

static void render_column(uint16_t x)
{
    for (uint16_t y = 0; y < PLOT_H; y++) col[y] = BH_BG;

    // Grid: 4 horizontal divisions (dotted, solid bottom line), one vertical dotted line every VGRID_EVERY points
    for (uint8_t k = 0; k <= 4; k++)
    {
        uint16_t y = (uint16_t)(k * (PLOT_H - 1) / 4);
        if (k == 4 || (x % 4) == 0) col[y] = BH_GRID;
    }
    uint16_t vgrid = (scale == TREND_FINE) ? 50 : 60;   // 5s / 1min
    if ((PLOT_W - 1 - x) % vgrid == 0)
        for (uint16_t y = 0; y < PLOT_H; y += 3) col[y] = BH_GRID;

    // Data is right-aligned: the newest point is in the rightmost column
    uint16_t n = trend_count(scale);
    if (x + n >= PLOT_W)
    {
        uint16_t i = (uint16_t)(x + n - PLOT_W);
        trend_pt_t p = trend_get(scale, i);
        trend_pt_t q = (i > 0) ? trend_get(scale, i - 1) : p;
        span(q.p1 + q.p2, p.p1 + p.p2, BH_ACCENT);
        span(q.p2, p.p2, BH_BLUE);
        span(q.p1, p.p1, BH_INK);
    }

    lcd_blit(PLOT_X + x, PLOT_Y, 1, PLOT_H, col);
}

static void draw_title(void)
{
    ui_draw_title(scale == TREND_FINE ? "TREND 30 S" : "TREND 5 MIN");
}

static void draw(void)
{
    gfx_fill_screen(BH_BG);
    draw_title();

    // Legend
    gfx_fill_rect(PLOT_X, 31, 10, 2, BH_INK);
    gfx_draw_string(PLOT_X + 12, 24, &font_8x16, "CH1", BH_INK, BH_BG);
    gfx_fill_rect(PLOT_X + 48, 31, 10, 2, BH_BLUE);
    gfx_draw_string(PLOT_X + 60, 24, &font_8x16, "CH2", BH_BLUE, BH_BG);
    gfx_fill_rect(PLOT_X + 96, 31, 10, 2, BH_ACCENT);
    gfx_draw_string(PLOT_X + 108, 24, &font_8x16, "OUT", BH_ACCENT, BH_BG);

    // FUI margin ticks: short ticks on both sides aligned with the horizontal grid, L-shaped brackets at the four corners
    for (uint8_t k = 0; k <= 4; k++)
    {
        uint16_t y = PLOT_Y + k * (PLOT_H - 1) / 4;
        gfx_hline(2, y, 6, BH_GRID);
        gfx_hline(PLOT_X + PLOT_W + 2, y, 6, BH_GRID);
    }
    gfx_vline(1, PLOT_Y, 8, BH_DIM);
    gfx_vline(1, PLOT_Y + PLOT_H - 8, 8, BH_DIM);
    gfx_vline(LCD_WIDTH - 2, PLOT_Y, 8, BH_DIM);
    gfx_vline(LCD_WIDTH - 2, PLOT_Y + PLOT_H - 8, 8, BH_DIM);

    range_lb = (label_t){.x = PLOT_X + 140, .y = 24, .font = &font_8x16, .width = 20, .right_align = true, .bg = BH_BG};
    axis_lb = (label_t){.x = PLOT_X, .y = 218, .font = &font_8x16, .width = 37, .bg = BH_BG};
    restart();
}

// When entered from the home page, show 5min first
static void enter(void)
{
    scale = TREND_COARSE;
    render_x = PLOT_W;
}

static void update(uint32_t now_ms)
{
    (void)now_ms;
    char buf[48], a[12], b[12];

    // A new point arrived and the previous pass has finished: redraw with the new data (peak and range update accordingly)
    if (trend_seq(scale) != rendered_seq && render_x >= PLOT_W) restart();

    for (uint8_t i = 0; i < COLS_PER_FRAME && render_x < PLOT_W; i++) render_column(render_x++);

    fmt_fixed(a, sizeof(a), peak, 1, 1);
    fmt_fixed(b, sizeof(b), full_scale, 1, 0);
    snprintf(buf, sizeof(buf), "0-%sW  PK %sW", b, a);
    label_set(&range_lb, buf, BH_DIM);

    const char *span_str = (scale == TREND_FINE) ? "-30S" : "-5MIN";
    snprintf(buf, sizeof(buf), "%s%*sNOW", span_str, (int)(34 - strlen(span_str)), "");
    label_set(&axis_lb, buf, BH_DIM);
}

static void on_key(key_evt_t evt)
{
    // Inspection page cycle: TREND 5min -> TREND 30s -> CAPS
    if (evt == KEY_EVT_CLICK)
    {
        if (scale == TREND_COARSE)
        {
            scale = TREND_FINE;
            draw_title();
            restart();
        }
        else
        {
            ui_goto(PAGE_CAPS);
        }
    }
    else if (evt == KEY_EVT_LONG)
    {
        ui_goto(PAGE_MENU);
    }
}

const ui_page_t page_trend = {draw, update, on_key, enter};
