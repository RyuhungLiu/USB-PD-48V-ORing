#include "page_bal.h"

#include <stdbool.h>
#include <stdio.h>
#include "bsp/delay.h"
#include "power/meter.h"
#include "power/share.h"
#include "ui/fmt.h"
#include "ui/gfx.h"
#include "ui/widget.h"

// Layout (320x240)
//  y=0    title bar
//  y=30   > MODE          NORMAL          16x24; color shows state: RUN green, switching / waiting orange, repeated failures red
//  y=56     status: center voltage / both channel currents / offset, or wait reason      8x16
//  y=74     description of the selected mode
//  y=100  > BACK
//  y=204  operation hints
//  y=224  TAP next  HOLD select
// Long press MODE to enter selection: TAP goes to the next, HOLD applies, 10s without action cancels

typedef enum item_t
{
    ITEM_MODE = 0,
    ITEM_BACK,
    ITEM_COUNT,
} item_t;

#define EDIT_TIMEOUT_MS 10000
#define BAL_OPTIONS     (SHARE_MODE_COUNT + 1)      // 0 = OFF, 1.. = share_mode_t + 1

static const uint16_t item_y[ITEM_COUNT] = {30, 100};

static uint8_t  cursor;
static bool     editing;
static uint8_t  edit_idx;
static uint32_t edit_last_ms;

static label_t item_lb[ITEM_COUNT];
static label_t state_lb, desc_lb, hint_lb;

static const char *option_str(uint8_t idx)
{
    return idx ? share_mode_str((share_mode_t)(idx - 1)) : "OFF";
}

static const char *option_desc(uint8_t idx)
{
    switch (idx)
    {
        case 1 + SHARE_MODE_NORMAL: return "equal current, 20mV / 0.5s";
        case 1 + SHARE_MODE_MANUAL: return share_learning() ? "learning, vary the load" : "learned feed-forward";
        case 1 + SHARE_MODE_FOLLOW: return "by PPS max current, may sag";
        default:                    return "fixed PDO, no current sharing";
    }
}

static void draw(void)
{
    ui_menu_frame("BALANCE", item_y[cursor]);
    for (uint8_t i = 0; i < ITEM_COUNT; i++)
        item_lb[i] = (label_t){.x = 8, .y = item_y[i], .font = &font_16x24, .width = 19,
                               .bg = (i == cursor) ? COLOR_NAVY : COLOR_BLACK};
    state_lb = (label_t){.x = 40, .y = 56, .font = &font_8x16, .width = 34, .bg = COLOR_BLACK};
    desc_lb = (label_t){.x = 40, .y = 74, .font = &font_8x16, .width = 34, .bg = COLOR_BLACK};
    hint_lb = (label_t){.x = 8, .y = 204, .font = &font_8x16, .width = 38, .bg = COLOR_BLACK};
}

static void update_state(void)
{
    char buf[48], v[12], a1[12], a2[12];
    share_state_t st = share_state();
    bool want = share_auto_on();

    if (st == SHARE_RUN)
    {
        fmt_fixed(v, sizeof(v), share_center_mv(), 3, 2);
        fmt_fixed(a1, sizeof(a1), meter_get(CH1)->current_ma_avg, 3, 2);
        fmt_fixed(a2, sizeof(a2), meter_get(CH2)->current_ma_avg, 3, 2);
        snprintf(buf, sizeof(buf), "%sV  %s/%sA  %+d/%+dmV", v, a1, a2,
                 share_offset_mv(CH1), share_offset_mv(CH2));
        label_set(&state_lb, buf, COLOR_GREEN);
    }
    else if (st != SHARE_OFF)
    {
        label_set(&state_lb, st == SHARE_ENTER ? "switching to PPS ..."
                           : st == SHARE_HOLD ? share_reason() : "back to fixed PDO ...", COLOR_YELLOW);
    }
    else if (want && share_wait_reason())
    {
        snprintf(buf, sizeof(buf), "wait: %s", share_wait_reason());
        label_set(&state_lb, buf, COLOR_YELLOW);
    }
    else if (share_reason())
    {
        snprintf(buf, sizeof(buf), "%s: %s", want ? "retry" : "off", share_reason());
        label_set(&state_lb, buf, want && !share_auto_gave_up() ? COLOR_YELLOW : COLOR_ORANGE);
    }
    else if (want)
    {
        label_set(&state_lb, share_auto_gave_up() ? "gave up, HOLD to choose again" : "waiting ...", COLOR_YELLOW);
    }
    else
    {
        label_set(&state_lb, "PPS current sharing", COLOR_GRAY);
    }
}

static void update(uint32_t now_ms)
{
    char buf[32];
    share_state_t st = share_state();
    bool want = share_auto_on();

    if (editing && (int32_t)(now_ms - edit_last_ms) > EDIT_TIMEOUT_MS)
    {
        editing = false;
        ui_toast("edit cancelled (timeout)", COLOR_GRAY, 1500);
    }

    if (editing)
    {
        snprintf(buf, sizeof(buf), "> MODE    [%s]", option_str(edit_idx));
        label_set(&item_lb[ITEM_MODE], buf, COLOR_ORANGE);
        label_set(&desc_lb, option_desc(edit_idx), COLOR_ORANGE);
        label_set(&hint_lb, "TAP next mode  HOLD apply", COLOR_ORANGE);
    }
    else
    {
        uint8_t cur = (want || st != SHARE_OFF) ? (uint8_t)(1 + share_mode()) : 0;
        uint16_t color = (st == SHARE_RUN) ? COLOR_GREEN
                       : (st != SHARE_OFF) ? COLOR_YELLOW
                       : !want ? COLOR_WHITE : share_auto_gave_up() ? COLOR_RED : COLOR_YELLOW;
        snprintf(buf, sizeof(buf), "%c MODE      %6s", cursor == ITEM_MODE ? '>' : ' ', option_str(cur));
        label_set(&item_lb[ITEM_MODE], buf, color);
        label_set(&desc_lb, option_desc(cur), COLOR_GRAY);
        label_set(&hint_lb, cursor == ITEM_MODE ? "HOLD to choose mode (saved)" : "", COLOR_GRAY);
    }
    update_state();

    snprintf(buf, sizeof(buf), "%c BACK", cursor == ITEM_BACK ? '>' : ' ');
    label_set(&item_lb[ITEM_BACK], buf, COLOR_WHITE);
}

// OFF turns it off; other options set the mode first and then enable (when already on, only the mode is switched)
static void apply(void)
{
    char buf[44];
    if (edit_idx == 0)
    {
        share_request(false);
        ui_toast("BALANCE OFF", COLOR_GRAY, 1500);
        return;
    }
    share_set_mode((share_mode_t)(edit_idx - 1));
    if (!share_auto_on() && !share_request(true))
    {
        snprintf(buf, sizeof(buf), "BALANCE %s, waiting: %s", share_mode_str(share_mode()), share_reason());
        ui_toast(buf, COLOR_YELLOW, 2500);
        return;
    }
    snprintf(buf, sizeof(buf), "BALANCE %s", share_mode_str(share_mode()));
    ui_toast(buf, COLOR_GREEN, 1500);
}

static void on_key(key_evt_t evt)
{
    if (editing)
    {
        edit_last_ms = millis();
        if (evt == KEY_EVT_CLICK) edit_idx = (uint8_t)((edit_idx + 1) % BAL_OPTIONS);
        else if (evt == KEY_EVT_LONG)
        {
            editing = false;
            apply();
        }
        return;
    }

    if (evt == KEY_EVT_CLICK)
    {
        cursor = ui_menu_next(item_lb, item_y, ITEM_COUNT, cursor);
        return;
    }
    if (evt != KEY_EVT_LONG) return;

    if (cursor == ITEM_MODE)
    {
        edit_idx = share_auto_on() ? (uint8_t)(1 + share_mode()) : 0;
        editing = true;
        edit_last_ms = millis();
    }
    else
    {
        cursor = 0;
        ui_goto(PAGE_MENU);
    }
}

static void enter(void)
{
    editing = false;
}

const ui_page_t page_bal = {draw, update, on_key, enter};
