#include "page_setup.h"

#include <stdio.h>
#include "app/settings.h"
#include "app/stats.h"
#include "bsp/delay.h"
#include "bsp/resetlog.h"
#include "power/meter.h"
#include "ui/fmt.h"
#include "ui/gfx.h"
#include "ui/widget.h"

// Layout (320x240)
//  y=0    title bar
//  y=26   > BOOT AUTO ON    OFF       16x24, cursor row dark blue background
//  y=50     target CH1 48V  CH2 48V   8x16
//  y=68   > RESET ENERGY
//  y=92     CH1 12.34Wh  CH2 0.56Wh   8x16
//  y=110  > RESET STATS               clear session statistics (confirm with another long press within 3s); the statistics page is in the inspection page cycle
//  y=134  > OCP LIMIT        110%     long press cycles 110/120/130/140% (percentage of contract current)
//  y=158  > BACK
//  y=184  last reset: cause + stuck stage     8x16
//  y=202  operation hint; storage status when there is no hint
//  y=224  TAP next  HOLD select

typedef enum item_t
{
    ITEM_AUTO_ON = 0,
    ITEM_RESET_ENERGY,
    ITEM_STATS,
    ITEM_OCP,
    ITEM_BACK,
    ITEM_COUNT,
} item_t;

#define CONFIRM_MS  3000    // clearing must be confirmed by another long press within this time

static const uint16_t item_y[ITEM_COUNT] = {26, 68, 110, 134, 158};

static uint8_t  cursor;
static uint32_t confirm_until;
static bool     confirm_pending;

static label_t item_lb[ITEM_COUNT];
static label_t target_lb, energy_lb, reset_lb, store_lb;

static void labels_init(void)
{
    for (uint8_t i = 0; i < ITEM_COUNT; i++)
    {
        item_lb[i] = (label_t){.x = 8, .y = item_y[i], .font = &font_16x24, .width = 19,
                               .bg = (i == cursor) ? COLOR_NAVY : COLOR_BLACK};
    }
    target_lb = (label_t){.x = 40, .y = 50,  .font = &font_8x16, .width = 35, .bg = COLOR_BLACK};
    energy_lb = (label_t){.x = 40, .y = 92,  .font = &font_8x16, .width = 35, .bg = COLOR_BLACK};
    reset_lb  = (label_t){.x = 8,  .y = 184, .font = &font_8x16, .width = 38, .bg = COLOR_BLACK};
    store_lb  = (label_t){.x = 8,  .y = 202, .font = &font_8x16, .width = 38, .bg = COLOR_BLACK};
}

static void draw(void)
{
    ui_menu_frame("SETUP", item_y[cursor]);
    labels_init();
}

static void fmt_target(char *buf, size_t size, uint16_t mv)
{
    if (mv) snprintf(buf, size, "%uV", mv / 1000);
    else snprintf(buf, size, "--");
}

static void update(uint32_t now_ms)
{
    const settings_t *s = settings_get();
    char buf[48], a[12], b[12];

    if (confirm_pending && (int32_t)(now_ms - confirm_until) >= 0) confirm_pending = false;

    snprintf(buf, sizeof(buf), "%c BOOT AUTO ON %s", cursor == ITEM_AUTO_ON ? '>' : ' ', s->auto_on ? " ON" : "OFF");
    label_set(&item_lb[ITEM_AUTO_ON], buf, s->auto_on ? COLOR_GREEN : COLOR_WHITE);

    fmt_target(a, sizeof(a), s->target_mv[CH1]);
    fmt_target(b, sizeof(b), s->target_mv[CH2]);
    snprintf(buf, sizeof(buf), "target CH1 %s  CH2 %s", a, b);
    label_set(&target_lb, buf, COLOR_GRAY);

    snprintf(buf, sizeof(buf), "%c RESET ENERGY", cursor == ITEM_RESET_ENERGY ? '>' : ' ');
    label_set(&item_lb[ITEM_RESET_ENERGY], buf, (confirm_pending && cursor == ITEM_RESET_ENERGY) ? COLOR_RED : COLOR_YELLOW);

    // Accumulated energy: mW*ms / 3.6e6 = mWh
    char e1[12], e2[12];
    fmt_fixed(e1, sizeof(e1), meter_mwms_to_mwh(meter_get(CH1)->energy_mwms), 3, 2);
    fmt_fixed(e2, sizeof(e2), meter_mwms_to_mwh(meter_get(CH2)->energy_mwms), 3, 2);
    snprintf(buf, sizeof(buf), "CH1 %sWh  CH2 %sWh", e1, e2);
    label_set(&energy_lb, buf, COLOR_GRAY);

    snprintf(buf, sizeof(buf), "%c RESET STATS", cursor == ITEM_STATS ? '>' : ' ');
    label_set(&item_lb[ITEM_STATS], buf, (confirm_pending && cursor == ITEM_STATS) ? COLOR_RED : COLOR_YELLOW);

    uint8_t next_ocp = (uint8_t)((s->ocp_level + 1) & 3);
    if (confirm_pending && cursor == ITEM_OCP)
    {
        snprintf(buf, sizeof(buf), "> OCP LIMIT  [%u%%]", 110 + 10 * next_ocp);
        label_set(&item_lb[ITEM_OCP], buf, COLOR_RED);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%c OCP LIMIT      %u%%", cursor == ITEM_OCP ? '>' : ' ', 110 + 10 * s->ocp_level);
        label_set(&item_lb[ITEM_OCP], buf, s->ocp_level ? COLOR_ORANGE : COLOR_WHITE);
    }

    snprintf(buf, sizeof(buf), "%c BACK", cursor == ITEM_BACK ? '>' : ' ');
    label_set(&item_lb[ITEM_BACK], buf, COLOR_WHITE);

    // On watchdog/software reset show the last stage executed in the previous run, to locate where it got stuck
    const rl_prev_t *rl = resetlog_prev();
    if (rl->valid)
    {
        fmt_fixed(a, sizeof(a), (int32_t)(rl->ms / 100), 1, 1);
        snprintf(buf, sizeof(buf), "RESET %s @%s %ss #%lu", sys_reset_cause_str(rl->cause),
                 resetlog_stage_str(rl->stage), a, rl->resets);
    }
    else
    {
        snprintf(buf, sizeof(buf), "RESET %s", sys_reset_cause_str(rl->cause));
    }
    label_set(&reset_lb, buf, rl->cause == RESET_CAUSE_IWDG ? COLOR_RED : COLOR_GRAY);

    // An operation hint occupies the storage status row
    char ocp_hint[32];
    snprintf(ocp_hint, sizeof(ocp_hint), "HOLD again: OCP %u%% -> %u%%", 110 + 10 * s->ocp_level, 110 + 10 * next_ocp);
    const char *hint = (confirm_pending && cursor == ITEM_OCP) ? ocp_hint
                     : (confirm_pending && cursor == ITEM_STATS) ? "HOLD again to clear session stats"
                     : confirm_pending                ? "HOLD again to clear energy"
                     : cursor == ITEM_AUTO_ON         ? "auto ON when PD = saved target"
                     : cursor == ITEM_RESET_ENERGY    ? "HOLD twice to clear Wh"
                     : cursor == ITEM_STATS           ? "HOLD twice: clear session stats"
                     : cursor == ITEM_OCP             ? "HOLD: OCP x contract current"
                                                      : NULL;
    uint32_t saved_ms = settings_last_save_ms();
    if (hint)
        snprintf(buf, sizeof(buf), "%s", hint);
    else if (settings_error())
        snprintf(buf, sizeof(buf), "STORE: WRITE ERROR (#%lu)", settings_seq());
    else if (saved_ms)
        snprintf(buf, sizeof(buf), "STORE: #%lu saved %lus ago", settings_seq(), (now_ms - saved_ms) / 1000);
    else
        snprintf(buf, sizeof(buf), "STORE: #%lu %s", settings_seq(), settings_loaded() ? "loaded" : "empty, defaults");
    label_set(&store_lb, buf, (confirm_pending || (!hint && settings_error())) ? COLOR_RED : COLOR_GRAY);
}

static void on_key(key_evt_t evt)
{
    if (evt == KEY_EVT_CLICK)
    {
        confirm_pending = false;
        cursor = ui_menu_next(item_lb, item_y, ITEM_COUNT, cursor);
        return;
    }
    if (evt != KEY_EVT_LONG) return;

    switch (cursor)
    {
        case ITEM_AUTO_ON:
        {
            bool on = !settings_get()->auto_on;
            settings_set_auto_on(on);
            ui_toast(on ? "AUTO ON: outputs turn on at boot" : "AUTO ON disabled", on ? COLOR_GREEN : COLOR_GRAY, 2000);
            break;
        }

        case ITEM_RESET_ENERGY:
            if (!confirm_pending)
            {
                confirm_pending = true;
                confirm_until = millis() + CONFIRM_MS;
                break;
            }
            confirm_pending = false;
            settings_reset_energy();
            ui_toast("energy cleared", COLOR_YELLOW, 1500);
            break;

        case ITEM_STATS:
            if (!confirm_pending)
            {
                confirm_pending = true;
                confirm_until = millis() + CONFIRM_MS;
                break;
            }
            confirm_pending = false;
            stats_reset_session(millis());
            ui_toast("session stats cleared", COLOR_YELLOW, 1500);
            break;

        case ITEM_OCP:
            // Protection related: takes effect only on another long press within 3s
            if (!confirm_pending)
            {
                confirm_pending = true;
                confirm_until = millis() + CONFIRM_MS;
                break;
            }
            confirm_pending = false;
            settings_set_ocp_level((uint8_t)((settings_get()->ocp_level + 1) & 3));
            break;

        case ITEM_BACK:
            cursor = 0;
            ui_goto(PAGE_MENU);
            break;

        default:
            break;
    }
}

static void enter(void)
{
    confirm_pending = false;
}

const ui_page_t page_setup = {draw, update, on_key, enter};
