#include "page_stats.h"

#include <stdio.h>
#include "config.h"
#include "app/stats.h"
#include "bsp/delay.h"
#include "power/meter.h"
#include "ui/fmt.h"
#include "ui/gfx.h"
#include "ui/widget.h"

// Layout (320x240), all 8x16
//  y=0    title bar
//  y=24   SESSION 01:23:45  BOOT 12  ON 123.4h     session duration / accumulated boot count / accumulated on time
//  y=42                  CH1         CH2
//  y=58   ON TIME   00:12:34    00:12:30
//  y=74   PEAK I       5.12A       3.01A
//  y=90   PEAK P      102.4W       60.2W
//  y=106  MAX T        45.2C       44.8C
//  y=122  TRIPS L/A      1/3         0/2           latched / auto-recoverable
//  y=138  ENERGY     12.34Wh     11.22Wh           accumulated (same as the SETUP page)
//  y=154  SUM PEAK 162.4W  BAL 00:10:00  TRIPS 9   combined peak / current sharing run time / accumulated trips
//  y=170  LAST CH1 OCP 5.62A LATCH  00:12 ago
//  y=188  MIN V    19.12V @5.1A  19.80V @3.0A    lowest VBUS while conducting and the current at that time
//  y=204  SAG<95%    3x 120ms      0x   0ms      count below 95% of contract voltage / longest duration
// Inspection page: TAP -> HOME, HOLD -> MENU; session statistics are cleared via RESET STATS in SETUP

#define ROWS        8       // data rows at y=24..170

static const uint16_t row_y[ROWS] = {24, 58, 74, 90, 106, 122, 138, 154};
static label_t row_lb[ROWS];
static label_t last_lb, sag_lb[2];

static void labels_init(void)
{
    for (uint8_t i = 0; i < ROWS; i++)
        row_lb[i] = (label_t){.x = 8, .y = row_y[i], .font = &font_8x16, .width = 38, .bg = COLOR_BLACK};
    last_lb = (label_t){.x = 8, .y = 170, .font = &font_8x16, .width = 38, .bg = COLOR_BLACK};
    for (uint8_t i = 0; i < 2; i++)
        sag_lb[i] = (label_t){.x = 8, .y = (uint16_t)(188 + 16 * i), .font = &font_8x16, .width = 38, .bg = COLOR_BLACK};
}

static void draw(void)
{
    gfx_fill_screen(COLOR_BLACK);
    ui_draw_title("STATISTICS");
    gfx_draw_string(8, 42, &font_8x16, "                 CH1         CH2", COLOR_GRAY, COLOR_BLACK);
    labels_init();
}

static void fmt_hms(char *buf, size_t size, uint32_t ms)
{
    uint32_t s = ms / 1000;
    snprintf(buf, size, "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
}

// Two-column value row: label + right-aligned CH1 / CH2
static void set_row(uint8_t row, const char *name, const char *a, const char *b)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%-10s%10s  %10s", name, a, b);
    label_set(&row_lb[row], buf, COLOR_WHITE);
}

static void update(uint32_t now_ms)
{
    const stats_t *s = stats_get();
    char buf[48], a[16], b[16], c[16];

    fmt_hms(a, sizeof(a), now_ms - s->since_ms);
    uint32_t on_s = stats_life_on_s();
    fmt_fixed(b, sizeof(b), (int32_t)(on_s / 360), 1, 1);     // 0.1h
    snprintf(buf, sizeof(buf), "SESSION %s  BOOT %lu  ON %sh", a, stats_life_boots(), b);
    label_set(&row_lb[0], buf, COLOR_CYAN);

    fmt_hms(a, sizeof(a), s->ch[CH1].on_ms);
    fmt_hms(b, sizeof(b), s->ch[CH2].on_ms);
    set_row(1, "ON TIME", a, b);

    fmt_fixed(a, sizeof(a), s->ch[CH1].peak_ma, 3, 2);
    fmt_fixed(b, sizeof(b), s->ch[CH2].peak_ma, 3, 2);
    snprintf(c, sizeof(c), "%sA", a);
    snprintf(a, sizeof(a), "%sA", b);
    set_row(2, "PEAK I", c, a);

    fmt_fixed(a, sizeof(a), s->ch[CH1].peak_mw, 3, 1);
    fmt_fixed(b, sizeof(b), s->ch[CH2].peak_mw, 3, 1);
    snprintf(c, sizeof(c), "%sW", a);
    snprintf(a, sizeof(a), "%sW", b);
    set_row(3, "PEAK P", c, a);

    fmt_fixed(a, sizeof(a), s->ch[CH1].max_temp_mc, 3, 1);
    fmt_fixed(b, sizeof(b), s->ch[CH2].max_temp_mc, 3, 1);
    snprintf(c, sizeof(c), "%sC", a);
    snprintf(a, sizeof(a), "%sC", b);
    set_row(4, "MAX T", c, a);

    snprintf(a, sizeof(a), "%u/%u", s->ch[CH1].trips_latched, s->ch[CH1].trips_auto);
    snprintf(b, sizeof(b), "%u/%u", s->ch[CH2].trips_latched, s->ch[CH2].trips_auto);
    set_row(5, "TRIPS L/A", a, b);

    // Accumulated energy: mW*ms / 3.6e6 = mWh
    fmt_fixed(a, sizeof(a), meter_mwms_to_mwh(meter_get(CH1)->energy_mwms), 3, 2);
    fmt_fixed(b, sizeof(b), meter_mwms_to_mwh(meter_get(CH2)->energy_mwms), 3, 2);
    snprintf(c, sizeof(c), "%sWh", a);
    snprintf(a, sizeof(a), "%sWh", b);
    set_row(6, "ENERGY", c, a);

    fmt_fixed(a, sizeof(a), s->peak_total_mw, 3, 1);
    fmt_hms(b, sizeof(b), s->share_run_ms);
    snprintf(buf, sizeof(buf), "SUM PEAK %sW  BAL %s  TRIPS %lu", a, b, stats_life_trips());
    label_set(&row_lb[7], buf, COLOR_WHITE);

    // Voltage sag: lowest voltage @ current at that time; count below contract by STATS_SAG_PCT% / longest duration
    char v1[24], v2[24];
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        const stats_ch_t *c = &s->ch[ch];
        char *o = ch ? v2 : v1;
        if (!c->min_mv)
        {
            snprintf(o, 24, "--");
            continue;
        }
        fmt_fixed(a, sizeof(a), c->min_mv, 3, 2);
        fmt_fixed(b, sizeof(b), c->min_ma, 3, 1);
        snprintf(o, 24, "%sV @%sA", a, b);
    }
    snprintf(buf, sizeof(buf), "%-6s%13s %13s", "MIN V", v1, v2);
    label_set(&sag_lb[0], buf, COLOR_WHITE);
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
        snprintf(ch ? v2 : v1, 24, "%ux %ums", s->ch[ch].sag_count, s->ch[ch].sag_max_ms);
    snprintf(buf, sizeof(buf), "SAG<%u%%%13s %13s", STATS_SAG_PCT, v1, v2);
    label_set(&sag_lb[1], buf, (s->ch[CH1].sag_count || s->ch[CH2].sag_count) ? COLOR_ORANGE : COLOR_WHITE);

    if (s->last_valid)
    {
        if (s->last_unit)
        {
            // unit is "mA"/"mV"/"mC": thousandth units, the m is dropped for display
            fmt_fixed(a, sizeof(a), s->last_value, 3, s->last_unit[1] == 'C' ? 1 : 2);
            snprintf(c, sizeof(c), " %s%c", a, s->last_unit[1]);
        }
        else
        {
            c[0] = '\0';
        }
        uint32_t ago = (now_ms - s->last_ms) / 1000;
        snprintf(buf, sizeof(buf), "LAST CH%d %s%s %s %02lu:%02lu ago", s->last_ch + 1, s->last_reason,
                 c, s->last_latched ? "LATCH" : "AUTO", ago / 60, ago % 60);
        label_set(&last_lb, buf, s->last_latched ? COLOR_RED : COLOR_ORANGE);
    }
    else
    {
        label_set(&last_lb, "LAST TRIP: none", COLOR_GRAY);
    }

}

static void on_key(key_evt_t evt)
{
    if (evt == KEY_EVT_CLICK) ui_goto(PAGE_HOME);
    else if (evt == KEY_EVT_LONG) ui_goto(PAGE_MENU);
}

const ui_page_t page_stats = {draw, update, on_key, NULL};
