#include "page_caps.h"

#include <stdio.h>
#include <string.h>
#include "pd/pd_node.h"
#include "ui/fmt.h"
#include "ui/gfx.h"
#include "ui/widget.h"

// Charger capability table (inspection page): CH1 left, CH2 right, one PDO per row; the current contract row has a dark purple background + EVA green text
// Layout (320x240)
//  y=0    title bar
//  y=24   CH1 EPR          240W       title: mode (EPR purple) <-> max power, or the reason EPR was not entered (orange)
//  y=41   ------------- thin line
//  y=44   1 FIX      5V     3A        position / type <-> voltage (right-aligned) <-> current / EPR AVS power (right-aligned)
//  ...    rows ordered by PDO position (16px per row, at most 11 rows): missing SPR positions left blank, EPR fixed starts at row 8;
//         SPR PDOs in white text, EPR PDOs in purple text, current contract dark purple background + EVA green text
// No labels: the whole column is redrawn when the capability table or contract changes (saves RAM)

#define COL_W       160
#define ROW_Y0      42
#define ROW_H       16

typedef struct col_key_t
{
    uint8_t  link;
    uint8_t  n_caps;
    uint8_t  caps_seq;
    uint8_t  pos;
    uint8_t  epr;
    uint8_t  valid;
    uint8_t  epr_note;
} col_key_t;

static col_key_t shown[CH_COUNT];
static bool      dirty;

static const char *kind_short(pdo_kind_t k)
{
    switch (k)
    {
        case PDO_KIND_FIXED:    return "FIX";
        case PDO_KIND_PPS:      return "PPS";
        case PDO_KIND_SPR_AVS:  return "AVS";
        case PDO_KIND_EPR_AVS:  return "EAV";
        case PDO_KIND_VARIABLE: return "VAR";
        case PDO_KIND_BATTERY:  return "BAT";
        default:                return "-";
    }
}

// mA -> "5" / "3.25" (whole amps without decimals)
static void fmt_a(char *buf, size_t size, uint16_t ma)
{
    if (ma % 1000 == 0) snprintf(buf, size, "%u", ma / 1000);
    else fmt_fixed(buf, size, ma, 3, 2);
}

// mV -> "20V" / "3.3V" (whole volts without decimals)
static void fmt_v(char *buf, size_t size, uint16_t mv)
{
    if (mv % 1000 == 0) snprintf(buf, size, "%u", mv / 1000);
    else fmt_fixed(buf, size, mv / 100, 1, 1);
}

// Maximum power in the capability table (W): fixed PDO VxI, PPS / SPR AVS take max voltage x current, EPR AVS takes PDP
static uint16_t max_power_w(const pd_node_t *n)
{
    uint32_t best = 0;
    for (uint8_t i = 0; i < n->n_caps && i < PD_MAX_CAPS; i++)
    {
        const pdo_info_t *p = &n->caps[i];
        uint32_t w = (p->kind == PDO_KIND_EPR_AVS) ? p->pdp_w : (uint32_t)p->max_mv * p->max_ma / 1000000u;
        if (w > best) best = w;
    }
    return (uint16_t)best;
}

// Each row: position number (gray) type voltage (right-aligned) current / power (right-aligned, against the column right edge); current contract dark purple background + EVA green text
static void draw_col(ch_id_t ch)
{
    const pd_node_t *n = pd_node_get(ch);
    uint16_t x0 = (uint16_t)(ch * COL_W);
    uint16_t x = (uint16_t)(x0 + 4);
    uint16_t xr = (uint16_t)(x0 + COL_W - 4);       // right-alignment edge (exclusive)
    char buf[24], a[8], b[8], c[12];

    gfx_fill_rect(x0, 24, COL_W, LCD_HEIGHT - 24, BH_BG);

    if (n->link != PD_LINK_ONLINE)
    {
        snprintf(buf, sizeof(buf), "CH%d PD OFFLINE", ch + 1);
        gfx_draw_string(x, 24, &font_8x16, buf, BH_RED, BH_BG);
        return;
    }
    if (!n->n_caps)
    {
        snprintf(buf, sizeof(buf), "CH%d %s", ch + 1, n->cc ? "no caps yet" : "unplugged");
        gfx_draw_string(x, 24, &font_8x16, buf, BH_DIM, BH_BG);
        return;
    }

    // Title: left channel + mode; right max power, or the reason when EPR is supported but not entered
    const char *note = NULL;
    if (n->src_epr_capable && !n->epr)
    {
        switch (n->epr_fail_reason)
        {
            case 0xFF: note = (n->has_auto_epr && !n->auto_epr) ? "EPR off" : "EPR-cap"; break;
            case 0x01: note = "EPR:cable"; break;
            case 0xFE: note = "EPR:reject"; break;
            default:   note = "EPR:fail"; break;
        }
    }
    snprintf(buf, sizeof(buf), "CH%d %s", ch + 1, n->epr ? "EPR" : "SPR");
    gfx_draw_string(x, 24, &font_8x16, buf, n->epr ? BH_BLUE : BH_DIM, BH_BG);
    if (note)
    {
        gfx_draw_string_right(xr, 24, 80, &font_8x16, note, BH_YELLOW, BH_BG);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%uW", max_power_w(n));
        gfx_draw_string_right(xr, 24, 80, &font_8x16, buf, BH_INK, BH_BG);
    }
    gfx_hline(x, 41, (uint16_t)(xr - x), BH_GRID);

    for (uint8_t i = 0; i < n->n_caps && i < PD_MAX_CAPS; i++)
    {
        const pdo_info_t *p = &n->caps[i];
        uint16_t y = (uint16_t)(ROW_Y0 + 2 + ROW_H * i);
        snprintf(buf, sizeof(buf), "%u", i + 1);
        gfx_draw_string_right((uint16_t)(x + 16), y, 16, &font_8x16, buf, BH_GRID, BH_BG);
        if (p->kind == PDO_KIND_NONE || p->max_mv == 0) continue;   // filler gap in the SPR part: leave blank
        bool cur = n->contract_valid && n->contract_pos == i + 1;
        uint16_t bg = cur ? COLOR_NAVY : BH_BG;
        uint16_t fg = cur ? BH_ACCENT : (i >= 7 ? BH_BLUE : BH_INK);

        fmt_v(a, sizeof(a), p->max_mv);
        if (p->min_mv != p->max_mv)
        {
            fmt_v(b, sizeof(b), p->min_mv);
            snprintf(buf, sizeof(buf), "%s-%sV", b, a);
        }
        else
        {
            snprintf(buf, sizeof(buf), "%sV", a);
        }
        if (p->kind == PDO_KIND_EPR_AVS) snprintf(c, sizeof(c), "%uW", p->pdp_w);
        else
        {
            fmt_a(c, sizeof(c), p->max_ma);
            snprintf(c + strlen(c), sizeof(c) - strlen(c), "A");
        }

        if (cur) gfx_fill_rect((uint16_t)(x + 18), y, (uint16_t)(xr - x - 16), ROW_H, bg);
        gfx_draw_string((uint16_t)(x + 20), y, &font_8x16, kind_short(p->kind), fg, bg);
        // Right alignment first fills the left side of the box with background: the box left edge must be after the type (x+20..x+44), otherwise it covers the 3rd character of the type
        gfx_draw_string_right((uint16_t)(x + 104), y, 60, &font_8x16, buf, fg, bg);
        gfx_draw_string_right(xr, y, 6 * 8, &font_8x16, c, fg, bg);
    }
}

static void draw(void)
{
    gfx_fill_screen(BH_BG);
    ui_draw_title("CHARGER CAPS");
    gfx_vline(COL_W, 24, LCD_HEIGHT - 24, BH_GRID);
    dirty = true;
}

static void update(uint32_t now_ms)
{
    (void)now_ms;
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        const pd_node_t *n = pd_node_get(ch);
        col_key_t k = {(uint8_t)n->link, n->n_caps, n->caps_seq, n->contract_pos, n->epr, n->contract_valid,
                       (uint8_t)(n->epr_fail_reason ^ (n->auto_epr ? 0x80 : 0))};
        if (!dirty && k.link == shown[ch].link && k.n_caps == shown[ch].n_caps && k.caps_seq == shown[ch].caps_seq &&
            k.pos == shown[ch].pos && k.epr == shown[ch].epr && k.valid == shown[ch].valid &&
            k.epr_note == shown[ch].epr_note)
            continue;
        shown[ch] = k;
        draw_col(ch);
        gfx_vline(COL_W, 24, LCD_HEIGHT - 24, BH_GRID);
    }
    dirty = false;
}

static void on_key(key_evt_t evt)
{
    if (evt == KEY_EVT_CLICK) ui_goto(PAGE_STATS);
    else if (evt == KEY_EVT_LONG) ui_goto(PAGE_MENU);
}

const ui_page_t page_caps = {draw, update, on_key, NULL};
