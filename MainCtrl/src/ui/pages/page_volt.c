#include "page_volt.h"

#include <stdbool.h>
#include <stdio.h>
#include "app/app.h"
#include "app/settings.h"
#include "bsp/delay.h"
#include "pd/pd_node.h"
#include "power/oring.h"
#include "ui/fmt.h"
#include "ui/gfx.h"
#include "ui/widget.h"

// Layout (320x240)
//  y=0    title bar
//  y=26   > SYNC BOTH CH     ON          16x24, cursor row dark purple background
//  y=52   > CH1 SET     48V              value being edited on an orange band []
//  y=76     now EPR FIX 48.00V 5.00A OK   8x16
//  y=94   > CH2 SET     48V
//  y=118    ...
//  y=136  > AUTO EPR           ON         enter EPR automatically when the charger supports it (voltage unchanged, EPR capability table becomes readable)
//  y=162  > BACK                         back to MENU
//  y=204  operation hint (changes with the cursor and edit state)
//  y=224  TAP next  HOLD select
//
// SYNC ON (default): changing either channel switches both together. The ORing is ideal diodes in parallel,
// when the two voltages differ the higher channel carries the whole load, so normal use should keep them in sync.
// While editing, only the fixed PDOs in the charger capability table are listed (with SYNC, those both channels have);
// when the charger supports EPR but has not entered EPR yet (EPR capability table unknown), 28 / 36 / 48V are listed as well

typedef enum item_t
{
    ITEM_SYNC = 0,
    ITEM_CH1,
    ITEM_CH2,
    ITEM_EPR,
    ITEM_BACK,
    ITEM_COUNT,
} item_t;

#define ITEM_TO_CH(i)   ((ch_id_t)((i) - ITEM_CH1))
#define CH_TO_ITEM(ch)  ((uint8_t)(ITEM_CH1 + (ch)))

#define EDIT_TIMEOUT_MS 10000

static const uint16_t item_y[ITEM_COUNT] = {26, 52, 94, 136, 162};
static const uint16_t steps_mv[] = {5000, 9000, 12000, 15000, 20000, 28000, 36000, 48000};
#define STEP_COUNT (sizeof(steps_mv) / sizeof(steps_mv[0]))

static uint8_t  cursor;
static bool     editing;
static uint8_t  edit_idx;               // position within opts
static uint32_t edit_last_ms;
static uint8_t  opt_n;
static uint16_t opts[STEP_COUNT];       // voltages selectable in this edit

static label_t item_lb[ITEM_COUNT];
static label_t info_lb[CH_COUNT];
static label_t hint_lb;

// Whether this channel's charger can provide a fixed PDO of mv; treated as yes when the capability table is unknown
static bool ch_offers(ch_id_t ch, uint16_t mv)
{
    const pd_node_t *n = pd_node_get(ch);
    if (!n->n_caps) return true;
    if (mv > 20000 && n->src_epr_capable && !n->epr) return true;   // the EPR capability table is known only after entering EPR
    for (uint8_t i = 0; i < n->n_caps; i++)
        if (n->caps[i].kind == PDO_KIND_FIXED && n->caps[i].max_mv == mv) return true;
    return false;
}

static void build_options(ch_id_t ch)
{
    bool sync = settings_get()->sync_on;
    opt_n = 0;
    for (uint8_t i = 0; i < STEP_COUNT; i++)
    {
        uint16_t mv = steps_mv[i];
        bool ok = sync ? (ch_offers(CH1, mv) && ch_offers(CH2, mv)) : ch_offers(ch, mv);
        if (ok) opts[opt_n++] = mv;
    }
    if (!opt_n) opts[opt_n++] = 5000;
}

static uint8_t nearest_opt(uint16_t mv)
{
    uint8_t best = 0;
    uint32_t best_diff = 0xFFFFFFFF;
    for (uint8_t i = 0; i < opt_n; i++)
    {
        uint32_t d = (mv > opts[i]) ? (uint32_t)(mv - opts[i]) : (uint32_t)(opts[i] - mv);
        if (d < best_diff) { best_diff = d; best = i; }
    }
    return best;
}

static void items_init(void)
{
    for (uint8_t i = 0; i < ITEM_COUNT; i++)
    {
        item_lb[i] = (label_t){.x = 8, .y = item_y[i], .font = &font_16x24, .width = 19,
                               .bg = (i == cursor) ? COLOR_NAVY : COLOR_BLACK};
    }
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        info_lb[ch] = (label_t){.x = 40, .y = item_y[CH_TO_ITEM(ch)] + 24, .font = &font_8x16,
                                .width = 34, .bg = COLOR_BLACK};
    }
    hint_lb = (label_t){.x = 8, .y = 204, .font = &font_8x16, .width = 38, .bg = COLOR_BLACK};
}

static void draw(void)
{
    ui_menu_frame("VOLTAGE", item_y[cursor]);
    items_init();
}

static void update_channel(ch_id_t ch)
{
    const pd_node_t *n = pd_node_get(ch);
    uint8_t item = CH_TO_ITEM(ch);
    bool sel = (cursor == item);
    char buf[48], v[12], a[12];

    if (editing && sel)
    {
        snprintf(buf, sizeof(buf), "> CH%d SET   [%2uV]", ch + 1, opts[edit_idx] / 1000);
        label_set(&item_lb[item], buf, COLOR_ORANGE);
    }
    else if (n->status_valid && n->preset_mv != 0 && n->preset_mv != 0xFFFF)
    {
        snprintf(buf, sizeof(buf), "%c CH%d SET    %2uV", sel ? '>' : ' ', ch + 1, n->preset_mv / 1000);
        label_set(&item_lb[item], buf, COLOR_WHITE);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%c CH%d SET    --", sel ? '>' : ' ', ch + 1);
        label_set(&item_lb[item], buf, COLOR_GRAY);
    }

    // Current contract + switching progress
    uint16_t color = COLOR_GRAY;
    if (n->link != PD_LINK_ONLINE)
    {
        snprintf(buf, sizeof(buf), "now: PD OFFLINE");
        color = COLOR_RED;
    }
    else if (n->contract_valid)
    {
        fmt_fixed(v, sizeof(v), n->contract_mv, 3, 2);
        fmt_fixed(a, sizeof(a), n->contract_ma, 3, 2);
        snprintf(buf, sizeof(buf), "now %s %s %sV %sA %s", n->epr ? "EPR" : "SPR",
                 pd_pdo_kind_str(n->contract_kind), v, a, pd_vset_str(n->vset));
    }
    else
    {
        snprintf(buf, sizeof(buf), "now: no contract seen %s", pd_vset_str(n->vset));
    }
    if (n->vset == PD_VSET_BUSY) color = COLOR_YELLOW;
    else if (n->vset == PD_VSET_OK) color = COLOR_GREEN;
    else if (n->vset == PD_VSET_FALLBACK) color = COLOR_RED;
    else if (n->vset == PD_VSET_STORED) color = COLOR_CYAN;
    if (app_vset_queued(ch))
    {
        snprintf(buf, sizeof(buf), "QUEUED: waits for other channel");
        color = COLOR_YELLOW;
    }
    label_set(&info_lb[ch], buf, color);
}

static void update(uint32_t now_ms)
{
    char buf[40];

    // Signed comparison: edit_last_ms takes millis() during key handling and may be a few ms later than this pass's now_ms,
    // unsigned subtraction would wrap to a huge value and immediately judge a timeout
    if (editing && (int32_t)(now_ms - edit_last_ms) > EDIT_TIMEOUT_MS)
    {
        editing = false;
        ui_toast("edit cancelled (timeout)", COLOR_GRAY, 1500);
    }

    snprintf(buf, sizeof(buf), "%c SYNC BOTH CH  %s", cursor == ITEM_SYNC ? '>' : ' ', settings_get()->sync_on ? " ON" : "OFF");
    label_set(&item_lb[ITEM_SYNC], buf, settings_get()->sync_on ? COLOR_GREEN : COLOR_ORANGE);

    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++) update_channel(ch);

    snprintf(buf, sizeof(buf), "%c AUTO EPR       %s", cursor == ITEM_EPR ? '>' : ' ', settings_get()->auto_epr ? " ON" : "OFF");
    label_set(&item_lb[ITEM_EPR], buf, settings_get()->auto_epr ? COLOR_GREEN : COLOR_ORANGE);

    snprintf(buf, sizeof(buf), "%c BACK", cursor == ITEM_BACK ? '>' : ' ');
    label_set(&item_lb[ITEM_BACK], buf, COLOR_WHITE);

    if (editing)
    {
        snprintf(buf, sizeof(buf), "%s: %u options  TAP next  HOLD apply", settings_get()->sync_on ? "BOTH" : "EDIT", opt_n);
        label_set(&hint_lb, buf, COLOR_ORANGE);
    }
    else if (cursor == ITEM_SYNC)
        label_set(&hint_lb, "HOLD to toggle sync", COLOR_GRAY);
    else if (cursor == ITEM_EPR)
        label_set(&hint_lb, "HOLD: enter EPR, keep voltage", COLOR_GRAY);
    else if (cursor == ITEM_BACK)
        label_set(&hint_lb, "", COLOR_GRAY);
    else if (oring_off_is_leaky())
        label_set(&hint_lb, "REV1: unplug load before switching", COLOR_RED);
    else
        label_set(&hint_lb, "HOLD to edit (MOS turns OFF)", COLOR_ORANGE);
}

// Turn off, save the target voltage, then send one channel at a time (see app_set_voltage)
static void apply(ch_id_t edited)
{
    uint16_t mv = opts[edit_idx];
    char buf[44];

    if (!settings_get()->sync_on)
    {
        if (app_set_voltage(1u << edited, mv))
            snprintf(buf, sizeof(buf), "CH%d -> %uV, MOS OFF, re-enable after OK", edited + 1, mv / 1000);
        else
            snprintf(buf, sizeof(buf), "CH%d denied: PD LINK OFFLINE", edited + 1);
        ui_toast(buf, COLOR_YELLOW, 3000);
        return;
    }

    uint8_t ok = app_set_voltage((1u << CH1) | (1u << CH2), mv);
    bool ok1 = ok & (1u << CH1), ok2 = ok & (1u << CH2);
    if (ok1 && ok2)
        snprintf(buf, sizeof(buf), "CH1 then CH2 -> %uV, MOS OFF", mv / 1000);
    else
        snprintf(buf, sizeof(buf), "-> %uV  CH1 %s  CH2 %s", mv / 1000, ok1 ? "ok" : "OFFLINE", ok2 ? "ok" : "OFFLINE");
    ui_toast(buf, (ok1 && ok2) ? COLOR_YELLOW : COLOR_RED, 3000);
}

static void on_key(key_evt_t evt)
{
    if (editing)
    {
        edit_last_ms = millis();
        if (evt == KEY_EVT_CLICK)
        {
            edit_idx = (uint8_t)((edit_idx + 1) % opt_n);
        }
        else if (evt == KEY_EVT_LONG)
        {
            editing = false;
            apply(ITEM_TO_CH(cursor));
        }
        return;
    }

    if (evt == KEY_EVT_CLICK)
    {
        cursor = ui_menu_next(item_lb, item_y, ITEM_COUNT, cursor);
        return;
    }
    if (evt != KEY_EVT_LONG) return;

    switch (cursor)
    {
        case ITEM_SYNC:
            settings_set_sync(!settings_get()->sync_on);
            ui_toast(settings_get()->sync_on ? "SYNC ON: both channels switch together" : "SYNC OFF: channels set separately",
                     settings_get()->sync_on ? COLOR_GREEN : COLOR_ORANGE, 2000);
            break;

        case ITEM_CH1:
        case ITEM_CH2:
        {
            ch_id_t ch = ITEM_TO_CH(cursor);
            const pd_node_t *n = pd_node_get(ch);
            uint16_t start = (n->status_valid && n->preset_mv && n->preset_mv != 0xFFFF) ? n->preset_mv
                           : (n->contract_valid ? n->contract_mv : 20000);
            build_options(ch);
            edit_idx = nearest_opt(start);
            editing = true;
            edit_last_ms = millis();
            break;
        }

        case ITEM_EPR:
            settings_set_auto_epr(!settings_get()->auto_epr);
            ui_toast(settings_get()->auto_epr ? "AUTO EPR ON" : "AUTO EPR OFF (takes effect on replug)",
                     settings_get()->auto_epr ? COLOR_GREEN : COLOR_ORANGE, 2000);
            break;

        case ITEM_BACK:
            cursor = 0;
            ui_goto(PAGE_MENU);
            break;

        default:
            break;
    }
}

// Cancel any unfinished edit when switching into the page (not called for the redraw when a hint expires, otherwise the edit would be silently cancelled)
static void enter(void)
{
    editing = false;
}

const ui_page_t page_volt = {draw, update, on_key, enter};
