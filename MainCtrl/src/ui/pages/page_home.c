#include "page_home.h"

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "app/stats.h"
#include "pd/pd_node.h"
#include "power/meter.h"
#include "power/oring.h"
#include "power/share.h"
#include "ui/fmt.h"
#include "ui/gfx.h"
#include "ui/widget.h"

// Visuals: EVA Unit-01 color scheme (purple + fluorescent green + amber warnings) -- pure black background, Chakra Petch Bold (2bpp anti-aliased), color blocks, strict grid,
//       states shown as circle / square / triangle; left and right halves split 1:1
// Interaction: FUI -- thin grid lines and ticks, corner brackets, scan line on page entry, light dots flowing with power
//
// Layout rules: each region has only one pair of left/right edges, every row is "left label <-> right value", large digits hug the right edge;
// the two lower rows on the right half are level with the MAX / current sharing rows of CH2
//
// Layout (320x240), vertical divider at x=160
//   Left half: two bands (119 rows each, divider at y=119), CH1 on top, CH2 below, no channel names; content L=12..R=150
//     x=0     status bar (white = conducting, yellow = waiting to recover, red = fault / offline, gray = off)
//     y+20    status shape (left) <-> large power digits (28px, "240.0" is 110px wide) + W (right)
//     y+60    voltage <-> current
//     y+82    thin tick line; when conducting a light dot flows at a speed set by power
//     y+92    current sharing: voltage offset <-> current share (red = current high, blue = current low)
//   Right half: content 169..311
//     y=6     CH1 MAX contract theoretical max power <-> protocol code (SF/SP/SA/EF/EA)
//     y=24    CH2 MAX ... ⟷ ...
//     y=42    BALANCE <-> status
//     y 64..177  EVA green block (centered on the left-half divider): OUT <-> W / large total power digits (vertically centered) / total current <-> output voltage
//     y=182   current difference between channels <-> current share bar
//     y=202   current sharing: CENTER <-> center voltage; otherwise PEAK <-> session peak power
//     y=220   current sharing: ADJUST <-> adjustment count; otherwise ENERGY <-> accumulated energy

#define HALF            160
#define BAND_H          119
#define BAND_Y(ch)      ((uint16_t)((ch) * (BAND_H + 1)))

#define L_X             12                  // left-half content edge
#define L_R             150
#define L_CHARS         ((L_R - L_X + 1) / 8)

#define R_X             (HALF + 9)          // right-half content edge
#define R_R             (LCD_WIDTH - 9)
#define R_CHARS         ((R_R - R_X + 1) / 8)

#define BLOCK_X         (HALF + 1)
#define BLOCK_W         (LCD_WIDTH - BLOCK_X)
#define BLOCK_Y         64
#define BLOCK_H         114                 // y 64..177, centered on the divider between the two left-half bands
#define BLK_X           (BLOCK_X + 10)      // color block content edge
#define BLK_R           (LCD_WIDTH - 11)

#define NUM_DY          20
#define LINE_DY         82
#define PULSE_W         12

#define BAR_W           56
#define BAR_H           12
#define BAR_Y           184                 // same row as the current difference at y=182, vertically centered

#define TINT_LEVELS     16
#define TINT_FULL_PCT   (SHARE_DIFF_PCT * 2)    // pure red / pure blue when the current difference reaches this ratio
#define INTRO_FRAMES    6                   // number of page-entry scan animation frames (x 50ms)

typedef enum band_state_t
{
    BS_NONE = 0,
    BS_OFF,
    BS_ON,
    BS_ARM,
    BS_FAULT,
    BS_OFFLINE,
} band_state_t;

// Large digit cache: redraw only when content or color changes
typedef struct num_t
{
    char     last[10];
    uint16_t fg;
    bool     drawn;
} num_t;

// One row: left label (left-aligned) + right value (right-aligned), sharing left/right edges
typedef struct row_t
{
    label_t l;
    label_t r;
} row_t;

typedef struct band_t
{
    num_t        pw;
    row_t        vi;
    row_t        bal;
    band_state_t state;
    uint16_t     pulse_x;                   // light dot offset relative to L_X
    bool         pulse_shown;
} band_t;

static band_t   band[CH_COUNT];
static num_t    total_num;
static row_t    blk_row;                    // color block bottom row: total current <-> output voltage
static row_t    top_row, bot1_row, bot2_row;
static row_t    max_row[CH_COUNT];          // upper right half: CHx MAX <-> protocol code
static label_t  diff_lb;
static label_t  msg_lb[2];                  // current sharing wait reason (full row, takes turns with bot1/bot2 at the same position)
static uint16_t bar_split = 0xFFFF, bar_c1, bar_c2;
static uint16_t bar_tick = 0xFFFF;              // follow PDO: tick at the target split position (0xFFFF = not drawn)
static uint8_t  intro = INTRO_FRAMES + 1;

static void num_set(num_t *n, uint16_t x_right, uint16_t y, uint16_t box_w, const font_t *font,
                    const char *text, uint16_t fg, uint16_t bg)
{
    if (n->drawn && n->fg == fg && strcmp(n->last, text) == 0) return;
    gfx_draw_string_right(x_right, y, box_w, font, text, fg, bg);
    snprintf(n->last, sizeof(n->last), "%s", text);
    n->fg = fg;
    n->drawn = true;
}

// Row width of chars characters; the rightmost r_chars characters are for the value
static void row_init(row_t *row, uint16_t x, uint16_t y, uint8_t chars, uint8_t r_chars, uint16_t bg)
{
    row->l = (label_t){.x = x, .y = y, .font = &font_8x16, .width = chars - r_chars, .bg = bg};
    row->r = (label_t){.x = x + (chars - r_chars) * 8, .y = y, .font = &font_8x16, .width = r_chars,
                       .right_align = true, .bg = bg};
}

static void row_set(row_t *row, const char *lt, uint16_t lc, const char *rt, uint16_t rc)
{
    label_set(&row->l, lt, lc);
    label_set(&row->r, rt, rc);
}

// ============================================================================
// Static elements
// ============================================================================

// FUI corner brackets: draw L-shaped thin lines at the four corners of a rectangle
static void brackets(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t len, uint16_t color)
{
    gfx_hline(x, y, len, color);
    gfx_vline(x, y, len, color);
    gfx_hline(x + w - len, y, len, color);
    gfx_vline(x + w - 1, y, len, color);
    gfx_hline(x, y + h - 1, len, color);
    gfx_vline(x, y + h - len, len, color);
    gfx_hline(x + w - len, y + h - 1, len, color);
    gfx_vline(x + w - 1, y + h - len, len, color);
}

static void draw_ticks(uint16_t y0)
{
    uint16_t y = y0 + LINE_DY, w = L_R - L_X + 1;
    gfx_hline(L_X, y, w, BH_GRID);
    for (uint16_t i = 0; i <= 10; i++)
    {
        uint16_t x = L_X + i * (w - 1) / 10;
        gfx_vline(x, y + 1, (i % 5 == 0) ? 5 : 2, BH_GRID);
    }
}

static void labels_init(void)
{
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        band_t *b = &band[ch];
        uint16_t y0 = BAND_Y(ch);
        b->pw = (num_t){0};
        row_init(&b->vi, L_X, y0 + 60, L_CHARS, 8, BH_BG);
        row_init(&b->bal, L_X, y0 + 92, L_CHARS, 5, BH_BG);
        row_init(&max_row[ch], R_X, 6 + ch * 18, R_CHARS, 3, BH_BG);
        b->state = BS_NONE;
        b->pulse_x = 0;
        b->pulse_shown = false;
    }
    total_num = (num_t){0};
    row_init(&blk_row, BLK_X, BLOCK_Y + BLOCK_H - 24, (BLK_R - BLK_X + 1) / 8, 8, BH_ACCENT);

    row_init(&top_row, R_X, 42, R_CHARS, 7, BH_BG);
    diff_lb = (label_t){.x = R_X, .y = 182, .font = &font_8x16, .width = 10, .bg = BH_BG};
    row_init(&bot1_row, R_X, 202, R_CHARS, 9, BH_BG);
    row_init(&bot2_row, R_X, 220, R_CHARS, 9, BH_BG);
    for (uint8_t i = 0; i < 2; i++)
        msg_lb[i] = (label_t){.x = R_X, .y = 202 + i * 18, .font = &font_8x16, .width = R_CHARS, .bg = BH_BG};
    bar_split = 0xFFFF;
    bar_tick = 0xFFFF;
}

static void draw_static(void)
{
    // Grid: divider between the two left-half bands + center line
    gfx_hline(0, BAND_H, HALF, BH_GRID);
    gfx_vline(HALF, 0, LCD_HEIGHT, BH_GRID);
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        draw_ticks(BAND_Y(ch));
        gfx_draw_string(L_R - 7, BAND_Y(ch) + NUM_DY + font_num_ch.height - 16, &font_8x16, "W", BH_DIM, BH_BG);
    }

    // Total output color block (vertically centered in the right half)
    gfx_fill_rect(BLOCK_X, BLOCK_Y, BLOCK_W, BLOCK_H, BH_ACCENT);
    brackets(BLOCK_X + 4, BLOCK_Y + 4, BLOCK_W - 8, BLOCK_H - 8, 10, BH_BG);
    gfx_draw_string(BLK_X, BLOCK_Y + 8, &font_8x16, "OUT", BH_BG, BH_ACCENT);
    gfx_draw_string(BLK_R - 7, BLOCK_Y + 8, &font_8x16, "W", BH_BG, BH_ACCENT);

    // Current share bar outline
    gfx_draw_rect(R_R - BAR_W, BAR_Y - 1, BAR_W + 2, BAR_H + 2, BH_GRID);
}

static void draw(void)
{
    gfx_fill_screen(BH_BG);
    labels_init();
    if (intro > INTRO_FRAMES) draw_static();    // redraw after a hint expires: do not replay the page-entry animation
}

static void enter(void)
{
    intro = 0;
}

// Page-entry scan: a bright line sweeps from top to bottom and grid lines extend along with it
static bool intro_step(void)
{
    if (intro > INTRO_FRAMES) return false;

    uint16_t prev_y = (uint16_t)((uint32_t)(intro ? intro - 1 : 0) * LCD_HEIGHT / INTRO_FRAMES);
    uint16_t y = (uint16_t)((uint32_t)intro * LCD_HEIGHT / INTRO_FRAMES);
    if (intro) gfx_hline(0, prev_y >= LCD_HEIGHT ? LCD_HEIGHT - 1 : prev_y, LCD_WIDTH, BH_BG);

    if (intro < INTRO_FRAMES)
    {
        gfx_hline(0, y, LCD_WIDTH, BH_INK);
        uint16_t len = (uint16_t)((uint32_t)(intro + 1) * HALF / INTRO_FRAMES);
        gfx_hline(0, BAND_H, len, BH_GRID);
        if (y) gfx_vline(HALF, 0, y, BH_GRID);
    }
    else
    {
        draw_static();
    }
    intro++;
    return true;
}

// ============================================================================
// Dynamic elements
// ============================================================================

static band_state_t band_state(ch_id_t ch)
{
    if (pd_node_get(ch)->link != PD_LINK_ONLINE) return BS_OFFLINE;
    if (oring_fault_latched(ch)) return BS_FAULT;
    if (oring_path_is_enabled(ch)) return BS_ON;
    if (oring_wants_on(ch)) return BS_ARM;
    return BS_OFF;
}

// Status bar + shape left of the digits: conducting ●, waiting to recover ○, off □, fault ▲, offline red □
static void draw_state(ch_id_t ch, band_state_t st)
{
    uint16_t y0 = BAND_Y(ch);
    uint16_t bar = (st == BS_ON) ? BH_INK : (st == BS_ARM) ? BH_YELLOW
                 : (st == BS_FAULT || st == BS_OFFLINE) ? BH_RED : BH_GRID;
    gfx_fill_rect(0, y0 + 10, 4, BAND_H - 20, bar);

    uint16_t cx = L_X + 6, cy = y0 + NUM_DY + font_num_ch.height / 2;
    gfx_fill_rect(cx - 8, cy - 8, 17, 17, BH_BG);
    switch (st)
    {
        case BS_ON:      gfx_fill_circle(cx, cy, 6, BH_INK); break;
        case BS_ARM:     gfx_ring(cx, cy, 6, 2, BH_YELLOW, BH_BG); break;
        case BS_FAULT:   gfx_fill_triangle(cx - 7, cy - 6, 14, BH_RED); break;
        case BS_OFFLINE: gfx_draw_rect(cx - 6, cy - 6, 13, 13, BH_RED); break;
        default:         gfx_draw_rect(cx - 6, cy - 6, 13, 13, BH_DIM); break;
    }
}

// Light dot flows along the tick line, faster with more power; hidden when not conducting
static void pulse_step(ch_id_t ch, bool on, int32_t power_mw)
{
    band_t *b = &band[ch];
    uint16_t y = BAND_Y(ch) + LINE_DY, w = L_R - L_X + 1;

    if (b->pulse_shown) gfx_hline(L_X + b->pulse_x, y, PULSE_W, BH_GRID);
    if (!on)
    {
        b->pulse_shown = false;
        return;
    }
    int32_t speed = 2 + power_mw / 10000;           // +1 pixel / frame per 10W
    if (speed > 12) speed = 12;
    b->pulse_x = (uint16_t)((b->pulse_x + speed) % (w - PULSE_W));
    gfx_hline(L_X + b->pulse_x, y, PULSE_W, BH_INK);
    b->pulse_shown = true;
}

static void fmt_watts(char *buf, size_t size, int32_t mw)
{
    int32_t w10 = (mw < 0) ? 0 : (mw + 50) / 100;  // 0.1W
    if (w10 > 9999) w10 = 9999;
    snprintf(buf, size, "%ld.%ld", w10 / 10, w10 % 10);
}

// Protocol code: first letter S/E = SPR/EPR, second letter F/P/A/V/B = fixed / PPS / AVS / variable / battery
static const char *contract_code(const pd_node_t *n)
{
    switch (n->contract_kind)
    {
        case PDO_KIND_FIXED:    return n->epr ? "EF" : "SF";
        case PDO_KIND_PPS:      return "SP";
        case PDO_KIND_EPR_AVS:  return "EA";
        case PDO_KIND_SPR_AVS:  return "SA";
        case PDO_KIND_VARIABLE: return n->epr ? "EV" : "SV";
        case PDO_KIND_BATTERY:  return n->epr ? "EB" : "SB";
        default:                return "--";
    }
}

static bool share_active(void)
{
    return share_state() != SHARE_OFF || share_auto_on();
}

static void update_band(ch_id_t ch, int32_t i_hi_ch, uint8_t tint, bool balancing)
{
    band_t *b = &band[ch];
    const meter_ch_t *m = meter_get(ch);
    const pd_node_t *n = pd_node_get(ch);
    band_state_t st = band_state(ch);
    char buf[24], v[12], a[12];

    if (st != b->state)
    {
        draw_state(ch, st);
        b->state = st;
    }

    // Large digits + W against the right edge (W takes 8px, 2px gap left at the digits' right edge)
    fmt_watts(buf, sizeof(buf), m->online ? m->power_mw_avg : 0);
    num_set(&b->pw, L_R - 9, BAND_Y(ch) + NUM_DY, L_R - 9 - (L_X + 16), &font_num_ch, buf,
            (st == BS_ON) ? BH_INK : BH_DIM, BH_BG);

    if (m->online)
    {
        fmt_fixed(v, sizeof(v), m->vbus_mv_avg, 3, 2);
        fmt_fixed(a, sizeof(a), m->current_ma_avg, 3, 2);
        snprintf(buf, sizeof(buf), "%sV", v);
        snprintf(v, sizeof(v), "%sA", a);
        row_set(&b->vi, buf, BH_DIM, v, BH_DIM);
    }
    else
    {
        row_set(&b->vi, "NO I-SENSE", BH_RED, "", BH_BG);
    }

    // Upper right half: CHx MAX contract theoretical max power (contract voltage x contract current, e.g. PPS 19.9V 5A -> 99.5W) <-> protocol code
    if (n->contract_valid && n->contract_mv)
    {
        int32_t d10 = (int32_t)((uint32_t)n->contract_mv * n->contract_ma / 100000);   // 0.1W
        fmt_fixed(v, sizeof(v), d10, 1, 1);
        snprintf(buf, sizeof(buf), "CH%d MAX %sW", ch + 1, v);
        row_set(&max_row[ch], buf, BH_DIM, contract_code(n), BH_INK);
    }
    else
    {
        snprintf(buf, sizeof(buf), "CH%d MAX --", ch + 1);
        row_set(&max_row[ch], buf, BH_GRID, "", BH_BG);
    }

    // Current sharing: voltage offset <-> current share; the higher-current channel turns red, the lower one blue, graded from white by current difference (tint 0..TINT_LEVELS)
    int32_t total = meter_total_current_ma();
    if (balancing && share_state() == SHARE_RUN && total > 0)
    {
        uint16_t c = (i_hi_ch < 0) ? BH_INK
                   : gfx_mix565(BH_INK, (i_hi_ch == (int32_t)ch) ? BH_RED : BH_BLUE, tint, TINT_LEVELS);
        if (share_anchor(ch)) snprintf(buf, sizeof(buf), "FIXED");
        else snprintf(buf, sizeof(buf), "%+dmV", share_offset_mv(ch));
        snprintf(v, sizeof(v), "%ld%%", m->current_ma_avg * 100 / total);
        row_set(&b->bal, buf, c, v, c);
    }
    else
    {
        row_set(&b->bal, "", BH_BG, "", BH_BG);
    }

    pulse_step(ch, st == BS_ON, m->power_mw_avg);
}

// Current share bar of both channels (against the right edge): left segment CH1, right segment CH2; during current sharing same as the left column, graded from white by current difference,
// the higher-current channel turns red, the lower one blue; when the two segment colors are close they are separated by a 1px black gap
static void update_bar(int32_t i1, int32_t i2, int32_t i_hi_ch, uint8_t tint, bool balancing)
{
    int32_t total = i1 + i2;

    // Follow PDO: a 3px tick below the bar marks the target split (the share CH1 should carry)
    uint16_t tick = 0xFFFF;
    if (balancing && share_mode() == SHARE_MODE_FOLLOW && total > 50)
    {
        int32_t t = share_target_ma(CH1, total) * BAR_W / total;
        tick = (uint16_t)(t < 0 ? 0 : t > BAR_W - 1 ? BAR_W - 1 : t);
    }
    if (tick != bar_tick)
    {
        uint16_t tx = R_R - BAR_W + 1;
        if (bar_tick != 0xFFFF) gfx_fill_rect(tx + bar_tick, BAR_Y + BAR_H + 2, 1, 3, BH_BG);
        if (tick != 0xFFFF) gfx_fill_rect(tx + tick, BAR_Y + BAR_H + 2, 1, 3, BH_INK);
        bar_tick = tick;
    }

    uint16_t split = (total > 50) ? (uint16_t)((i1 < 0 ? 0 : i1) * BAR_W / total) : BAR_W / 2;
    if (split > BAR_W) split = BAR_W;

    uint16_t c1 = BH_DIM, c2 = BH_GRID;
    if (balancing)
    {
        uint8_t k = (i_hi_ch >= 0) ? tint : 0;
        c1 = gfx_mix565(BH_INK, (i_hi_ch == CH1) ? BH_RED : BH_BLUE, k, TINT_LEVELS);
        c2 = gfx_mix565(BH_INK, (i_hi_ch == CH1) ? BH_BLUE : BH_RED, k, TINT_LEVELS);
    }
    if (split == bar_split && c1 == bar_c1 && c2 == bar_c2) return;

    uint16_t x = R_R - BAR_W + 1;
    if (split) gfx_fill_rect(x, BAR_Y, split, BAR_H, c1);
    if (split < BAR_W) gfx_fill_rect(x + split, BAR_Y, BAR_W - split, BAR_H, c2);
    if (balancing && split > 0 && split < BAR_W) gfx_fill_rect(x + split, BAR_Y, 1, BAR_H, BH_BG);
    bar_split = split;
    bar_c1 = c1;
    bar_c2 = c2;
}

// Upper and lower areas of the right half
static void update_side(bool balancing, int32_t total, int32_t diff)
{
    char buf[24], v[16];
    share_state_t st = share_state();

    bool waiting = balancing && st != SHARE_RUN;
    if (!waiting)
    {
        // Clear the wait reason first, then draw the value rows (both at the same position)
        label_set(&msg_lb[0], "", BH_BG);
        label_set(&msg_lb[1], "", BH_BG);
    }

    if (!balancing)
    {
        row_set(&top_row, "BALANCE", BH_DIM, "OFF", BH_GRID);
        if (oring_overload()) label_set(&diff_lb, "OVERLOAD", BH_RED);
        else label_set(&diff_lb, "DIFF --", BH_GRID);

        fmt_fixed(v, sizeof(v), stats_get()->peak_total_mw, 3, 1);
        snprintf(buf, sizeof(buf), "%sW", v);
        row_set(&bot1_row, "PEAK", BH_DIM, buf, BH_DIM);
        // Accumulated energy: mW*ms / 3.6e6 = mWh
        uint64_t mwms = meter_get(CH1)->energy_mwms + meter_get(CH2)->energy_mwms;
        fmt_fixed(v, sizeof(v), meter_mwms_to_mwh(mwms), 3, 2);
        snprintf(buf, sizeof(buf), "%sWh", v);
        row_set(&bot2_row, "ENERGY", BH_DIM, buf, BH_DIM);
        return;
    }

    const char *s = (st == SHARE_RUN) ? "RUN" : (st == SHARE_OFF) ? (share_auto_gave_up() ? "GIVEUP" : "WAIT")
                  : share_state_str(st);
    const char *title = share_learning() ? "BAL LEARN"
                      : (share_mode() == SHARE_MODE_MANUAL) ? (share_manual_ff() ? "BAL MANUAL" : "BAL MAN-NO")
                      : (share_mode() == SHARE_MODE_FOLLOW) ? "BAL FOLLOW" : "BALANCE";
    row_set(&top_row, title, BH_DIM, s,
            (st == SHARE_RUN) ? BH_INK : (share_auto_gave_up() ? BH_RED : BH_YELLOW));

    if (st == SHARE_RUN)
    {
        int32_t pct10 = (total > 0) ? (diff < 0 ? -diff : diff) * 1000 / total : 0;   // 0.1%
        snprintf(buf, sizeof(buf), "DIFF %ld.%ld%%", pct10 / 10, pct10 % 10);
        // Overload: the sum of conducting channel currents exceeds 97% of the summed contracts (any split would overload one channel); the DIFF position shows a warning instead
        if (oring_overload()) label_set(&diff_lb, "OVERLOAD", BH_RED);
        else label_set(&diff_lb, buf, BH_INK);

        fmt_fixed(v, sizeof(v), share_center_mv(), 3, 2);
        snprintf(buf, sizeof(buf), "%sV", v);
        row_set(&bot1_row, "CENTER", BH_DIM, buf, BH_DIM);
        snprintf(buf, sizeof(buf), "%lu", share_adjust_count());
        row_set(&bot2_row, "ADJUST", BH_DIM, buf, BH_DIM);
    }
    else
    {
        label_set(&diff_lb, "DIFF --", BH_GRID);
        // Clear the value rows first, then draw the reason (full row width, wrapped to two rows if too long)
        row_set(&bot1_row, "", BH_BG, "", BH_BG);
        row_set(&bot2_row, "", BH_BG, "", BH_BG);
        const char *why = share_wait_reason() ? share_wait_reason() : share_reason();
        char l1[R_CHARS + 1] = "", l2[R_CHARS + 1] = "";
        if (why)
        {
            snprintf(l1, sizeof(l1), "%s", why);
            if (strlen(why) > R_CHARS) snprintf(l2, sizeof(l2), "%s", why + R_CHARS);
        }
        label_set(&msg_lb[0], l1, BH_YELLOW);
        label_set(&msg_lb[1], l2, BH_YELLOW);
    }
}

static void update(uint32_t now_ms)
{
    (void)now_ms;
    if (intro_step()) return;

    bool balancing = share_active();
    int32_t i1 = meter_get(CH1)->current_ma_avg, i2 = meter_get(CH2)->current_ma_avg;
    int32_t total = i1 + i2, diff = i1 - i2;
    // During current sharing the target split is the reference: diff = 2 x (CH1 actual - CH1 target), i.e. i1 - i2 when split evenly; follow PDO uses the contract current ratio
    if (balancing) diff = 2 * (i1 - share_target_ma(CH1, total));
    // The channel with clearly higher current (difference > 1% of total current), otherwise -1
    int32_t i_hi_ch = (total > 0 && (diff < 0 ? -diff : diff) * 100 > total) ? (diff > 0 ? CH1 : CH2) : -1;
    // Color shift of the left-column current sharing row: current difference as share of total current 0 -> white, >= TINT_FULL_PCT -> pure red / pure blue, TINT_LEVELS steps in between (redrawn once per step)
    uint8_t tint = 0;
    if (total > 0)
    {
        int32_t t = (diff < 0 ? -diff : diff) * TINT_LEVELS * 100 / (total * TINT_FULL_PCT);
        tint = (uint8_t)(t > TINT_LEVELS ? TINT_LEVELS : t);
    }

    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++) update_band(ch, i_hi_ch, tint, balancing);

    // Total output: power (against the color block right edge) + total current <-> output voltage (highest VBUS among conducting channels; ideal diodes in parallel take the highest)
    char buf[16], a[12], v[16];
    fmt_watts(buf, sizeof(buf), meter_total_power_mw());
    num_set(&total_num, BLK_R + 1, BLOCK_Y + (BLOCK_H - font_num_out.height) / 2, BLK_R + 1 - BLK_X,
            &font_num_out, buf, BH_BG, BH_ACCENT);

    int32_t vout = 0;
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        const meter_ch_t *m = meter_get(ch);
        if (m->online && oring_path_is_enabled(ch) && m->vbus_mv_avg > vout) vout = m->vbus_mv_avg;
    }
    fmt_fixed(a, sizeof(a), meter_total_current_ma(), 3, 2);
    snprintf(buf, sizeof(buf), "%sA", a);
    if (vout)
    {
        fmt_fixed(a, sizeof(a), vout, 3, 2);
        snprintf(v, sizeof(v), "%sV", a);
    }
    else
    {
        snprintf(v, sizeof(v), "--V");
    }
    row_set(&blk_row, buf, BH_BG, v, BH_BG);

    update_bar(i1, i2, i_hi_ch, tint, balancing);
    update_side(balancing, total, diff);
}

static void on_key(key_evt_t evt)
{
    if (evt == KEY_EVT_CLICK) ui_goto(PAGE_TREND);
    else if (evt == KEY_EVT_LONG) ui_goto(PAGE_MENU);
    // Triple click: turn on all MOS (double click already turned all off first); if both channels were already conducting before the triple click, do not turn on again, avoiding a momentary interruption
    else if (evt == KEY_EVT_TRIPLE && !ui_double_was_all_on()) ui_all_mos_on();
}

const ui_page_t page_home = {draw, update, on_key, enter};
