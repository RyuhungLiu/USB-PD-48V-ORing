#include "page_mos.h"

#include <stdio.h>
#include "config.h"
#include "bsp/delay.h"
#include "pd/pd_node.h"
#include "power/meter.h"
#include "power/oring.h"
#include "ui/fmt.h"
#include "ui/gfx.h"
#include "ui/widget.h"

// Layout (320x240)
//  y=0    title bar
//  y=30   > CH1 MOS          OFF      16x24, cursor row dark blue background
//  y=56     PD ON 15.00V  0.02A /3.30A     8x16, when off shows the reason and the measured value at that time
//  y=80   > CH2 MOS          OFF
//  y=106    ...
//  y=130  > ALL ON                       latched faults are not cleared
//  y=156  > ALL OFF
//  y=182  >REPLUG 1   >REPLUG 2          software replug (X035 2.2+ ErrorRecovery), both items on one row; executed only on a second long press within 3s
//  y=208  > BACK                         back to MENU
//  y=224  protection status / hardware warning

typedef enum item_t
{
    ITEM_CH1 = 0,
    ITEM_CH2,
    ITEM_ALL_ON,
    ITEM_ALL_OFF,
    ITEM_REPLUG1,
    ITEM_REPLUG2,
    ITEM_BACK,
    ITEM_COUNT,
} item_t;

static const uint16_t item_y[ITEM_COUNT] = {30, 80, 130, 156, 182, 182, 208};
static const uint16_t item_x[ITEM_COUNT] = {8, 8, 8, 8, 8, 168, 8};
static const uint8_t  item_w[ITEM_COUNT] = {19, 19, 19, 19, 9, 9, 19};

#define CONFIRM_MS  3000    // software replug must be confirmed by another long press within this time

static uint8_t cursor;
static uint8_t  confirm_item = ITEM_COUNT;     // REPLUG item awaiting confirmation, ITEM_COUNT = none
static uint32_t confirm_until;
static label_t item_lb[ITEM_COUNT];
static label_t info_lb[CH_COUNT];
static label_t prot_lb;

static void items_init(void)
{
    for (uint8_t i = 0; i < ITEM_COUNT; i++)
    {
        item_lb[i] = (label_t){.x = item_x[i], .y = item_y[i], .font = &font_16x24, .width = item_w[i],
                               .bg = (i == cursor) ? COLOR_NAVY : COLOR_BLACK};
    }
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        info_lb[ch] = (label_t){.x = 40, .y = item_y[ch] + 26, .font = &font_8x16, .width = 35, .bg = COLOR_BLACK};
    }
    prot_lb = (label_t){.x = 8, .y = 224, .font = &font_8x16, .width = 38, .bg = COLOR_BLACK};
}

static void draw(void)
{
    gfx_fill_screen(COLOR_BLACK);
    ui_draw_title("OUTPUT");

    items_init();
}

static void move_cursor(void)
{
    confirm_item = ITEM_COUNT;
    // Old cursor item back to black background, new cursor item dark blue background; full-row items fill the row, the two REPLUG items fill half a row each
    for (uint8_t k = 0; k < 2; k++)
    {
        if (k) cursor = (cursor + 1) % ITEM_COUNT;
        uint16_t bg = k ? COLOR_NAVY : COLOR_BLACK;
        bool half = item_w[cursor] < 19;
        item_lb[cursor].bg = bg;
        gfx_fill_rect(half ? item_x[cursor] - 8 : 0, item_y[cursor], half ? LCD_WIDTH / 2 : LCD_WIDTH, 24, bg);
        label_invalidate(&item_lb[cursor]);
    }
}

// Bottom row: hardware warning first, then missing INA, otherwise show that protection is enabled
static void update_prot_line(void)
{
    if (oring_hw() == ORING_HW_UNKNOWN)
    {
        label_set(&prot_lb, "HW REV UNKNOWN: MOS control locked", COLOR_RED);
        return;
    }
    if (oring_off_is_leaky())
    {
        label_set(&prot_lb, "REV1: OFF still outputs VIN-3V!", COLOR_RED);
        return;
    }
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        if (!meter_get(ch)->online)
        {
            char buf[40];
            snprintf(buf, sizeof(buf), PROT_REQUIRE_ISENSE ? "CH%d NO I-SENSE: turn-on locked"
                                                           : "CH%d NO I-SENSE: no protection!", ch + 1);
            label_set(&prot_lb, buf, COLOR_RED);
            return;
        }
    }
    label_set(&prot_lb, "PROT: OCP SCP OVP UVP OTP REV", COLOR_GRAY);
}

// Trip reason + measured value at that time, e.g. "OCP 1.62A"; returns an empty string if no record
static void trip_text(ch_id_t ch, char *buf, size_t size)
{
    const char *reason = oring_last_trip(ch);
    int32_t value;
    const char *unit;
    char num[12];

    if (!reason)
    {
        buf[0] = '\0';
        return;
    }
    if (!oring_last_trip_value(ch, &value, &unit, NULL) || !unit)
    {
        snprintf(buf, size, "%s", reason);
        return;
    }
    // unit is "mA"/"mV"/"mC": thousandth units, the m is dropped for display
    fmt_fixed(num, sizeof(num), value, 3, unit[1] == 'C' ? 1 : 2);
    snprintf(buf, size, "%s %s%c", reason, num, unit[1]);
}

static void update_channel(ch_id_t ch)
{
    char buf[48], v[12], i[12], lim[12], trip_buf[24];
    bool on = oring_path_is_enabled(ch);
    bool latched = oring_fault_latched(ch);
    bool waiting = !on && oring_wants_on(ch);

    const char *state = latched ? "FAULT" : on ? "   ON" : waiting ? " WAIT" : "  OFF";
    uint16_t state_color = latched ? COLOR_RED : on ? COLOR_GREEN : waiting ? COLOR_YELLOW : COLOR_WHITE;
    snprintf(buf, sizeof(buf), "%c CH%d MOS     %s", cursor == ch ? '>' : ' ', ch + 1, state);
    label_set(&item_lb[ch], buf, state_color);

    const pd_node_t *n = pd_node_get(ch);
    const meter_ch_t *m = meter_get(ch);
    trip_text(ch, trip_buf, sizeof(trip_buf));
    const char *trip = trip_buf[0] ? trip_buf : NULL;
    const char *link = n->link == PD_LINK_ONLINE ? "ON " : "OFF";

    if (latched)
    {
        snprintf(buf, sizeof(buf), "%s  HOLD to clear", trip ? trip : "FAULT");
        label_set(&info_lb[ch], buf, COLOR_RED);
        return;
    }
    if (!m->online)
    {
        snprintf(buf, sizeof(buf), "PD %s NO I-SENSE %s", link, trip ? trip : "");
        label_set(&info_lb[ch], buf, COLOR_RED);
        return;
    }

    fmt_fixed(v, sizeof(v), m->vbus_mv_avg, 3, 2);
    fmt_fixed(i, sizeof(i), m->current_ma_avg, 3, 2);
    fmt_fixed(lim, sizeof(lim), oring_ocp_limit_ma(ch), 3, 1);
    if (trip)
        snprintf(buf, sizeof(buf), "PD %s %6sV %s", link, v, trip);   // not wide enough, current omitted
    else
        snprintf(buf, sizeof(buf), "PD %s %6sV %5sA /%sA%s", link, v, i, lim, n->contract_valid ? "" : "?");
    label_set(&info_lb[ch], buf, trip ? COLOR_ORANGE : (n->link == PD_LINK_ONLINE ? COLOR_GRAY : COLOR_RED));
}

static void update(uint32_t now_ms)
{
    char buf[48];

    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++) update_channel(ch);
    update_prot_line();

    snprintf(buf, sizeof(buf), "%c ALL ON", cursor == ITEM_ALL_ON ? '>' : ' ');
    label_set(&item_lb[ITEM_ALL_ON], buf, COLOR_GREEN);

    snprintf(buf, sizeof(buf), "%c ALL OFF", cursor == ITEM_ALL_OFF ? '>' : ' ');
    label_set(&item_lb[ITEM_ALL_OFF], buf, COLOR_YELLOW);

    if (confirm_item != ITEM_COUNT && (int32_t)(now_ms - confirm_until) >= 0) confirm_item = ITEM_COUNT;
    for (uint8_t k = 0; k < 2; k++)
    {
        snprintf(buf, sizeof(buf), "%cREPLUG %u", cursor == ITEM_REPLUG1 + k ? '>' : ' ', k + 1);
        label_set(&item_lb[ITEM_REPLUG1 + k], buf, confirm_item == ITEM_REPLUG1 + k ? COLOR_RED : COLOR_ORANGE);
    }

    snprintf(buf, sizeof(buf), "%c BACK", cursor == ITEM_BACK ? '>' : ' ');
    label_set(&item_lb[ITEM_BACK], buf, COLOR_WHITE);
}

static void on_key(key_evt_t evt)
{
    if (evt == KEY_EVT_CLICK)
    {
        move_cursor();
        return;
    }
    if (evt != KEY_EVT_LONG) return;

    switch (cursor)
    {
        case ITEM_CH1:
        case ITEM_CH2:
        {
            ch_id_t ch = (ch_id_t)cursor;
            char buf[40];
            if (oring_fault_latched(ch))
            {
                // Latched fault: one long press only clears it, another long press turns on
                oring_clear_fault(ch);
                snprintf(buf, sizeof(buf), "CH%d fault cleared, HOLD to turn on", ch + 1);
                ui_toast(buf, COLOR_YELLOW, 2500);
                break;
            }
            // A channel waiting for auto-recovery counts as "on"; long press cancels
            bool want_on = !(oring_path_is_enabled(ch) || oring_wants_on(ch));
            oring_deny_t deny = oring_request(ch, want_on);
            if (deny != ORING_OK)
            {
                snprintf(buf, sizeof(buf), "CH%d denied: %s", ch + 1, oring_deny_str(deny));
                ui_toast(buf, COLOR_RED, 2500);
            }
            break;
        }
        case ITEM_ALL_ON:
            ui_all_mos_on();
            break;
        case ITEM_ALL_OFF:
            oring_all_off("user menu");
            break;
        case ITEM_REPLUG1:
        case ITEM_REPLUG2:
        {
            ch_id_t ch = (ch_id_t)(cursor - ITEM_REPLUG1);
            char buf[40];
            // The first long press only shows a hint; executed only on another long press of the same item within CONFIRM_MS (an accidental touch under load would cut that channel for seconds)
            if (confirm_item != cursor || (int32_t)(millis() - confirm_until) >= 0)
            {
                confirm_item = cursor;
                confirm_until = millis() + CONFIRM_MS;
                snprintf(buf, sizeof(buf), "HOLD again to replug CH%d", ch + 1);
                ui_toast(buf, COLOR_YELLOW, CONFIRM_MS);
                break;
            }
            confirm_item = ITEM_COUNT;
            oring_deny_t deny = oring_replug(ch);
            if (deny != ORING_OK) snprintf(buf, sizeof(buf), "CH%d replug denied: %s", ch + 1, oring_deny_str(deny));
            else snprintf(buf, sizeof(buf), "CH%d replugging, wait for PD", ch + 1);
            ui_toast(buf, deny != ORING_OK ? COLOR_RED : COLOR_YELLOW, 2500);
            break;
        }
        case ITEM_BACK:
            cursor = 0;
            ui_goto(PAGE_MENU);
            break;
        default:
            break;
    }
}

const ui_page_t page_mos = {draw, update, on_key, NULL};
