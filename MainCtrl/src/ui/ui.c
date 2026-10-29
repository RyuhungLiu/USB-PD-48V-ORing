#include "ui.h"

#include <stdbool.h>
#include <stdio.h>
#include "bsp/delay.h"
#include "bsp/log.h"
#include "power/oring.h"
#include "ui/gfx.h"
#include "ui/widget.h"
#include "ui/pages/page_caps.h"
#include "ui/pages/page_home.h"
#include "ui/pages/page_trend.h"
#include "ui/pages/page_mos.h"
#include "ui/pages/page_volt.h"
#include "ui/pages/page_setup.h"
#include "ui/pages/page_stats.h"
#include "ui/pages/page_menu.h"
#include "ui/pages/page_bal.h"

static const ui_page_t *pages[PAGE_COUNT];
static ui_page_id_t current;
static bool need_draw;
static bool need_enter;
static bool title_shown;     // the current page drew a title bar (the home page does not); only then is the MOS status on the right refreshed

// MOS status on the right of the title bar
static label_t path_lb[CH_COUNT];

// Bottom hint (overlaid on the footer)
#define TOAST_Y (LCD_HEIGHT - 16)
static label_t toast_lb;
static uint32_t toast_until;
static bool toast_active;
static bool double_all_on;   // both channels were conducting before the latest double-click all-off

void ui_draw_title(const char *title)
{
    title_shown = true;
    // Title bar: black background, EVA green square on the left, thin line at the bottom
    gfx_fill_rect(0, 0, LCD_WIDTH, UI_TITLE_H, BH_BG);
    gfx_fill_rect(0, 3, 5, 15, BH_ACCENT);
    gfx_draw_string(12, 3, &font_8x16, title, BH_INK, BH_BG);
    gfx_hline(0, UI_TITLE_H - 1, LCD_WIDTH, BH_GRID);

    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        path_lb[ch] = (label_t){.x = 200 + ch * 60, .y = 3, .font = &font_8x16, .width = 7, .bg = BH_BG};
    }
}

static void update_title(void)
{
    // First hardware revision still outputs when OFF: OFF shown as orange OFF*; hardware not identified shows LCK; latched fault FLT; waiting to recover ARM
    char buf[8] = "CHx:OFF";
    bool leaky = oring_off_is_leaky();
    bool locked = (oring_hw() == ORING_HW_UNKNOWN);
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        bool on = oring_path_is_enabled(ch);
        buf[2] = '1' + ch;
        if (locked)
        {
            buf[4] = 'L'; buf[5] = 'C'; buf[6] = 'K';
            label_set(&path_lb[ch], buf, COLOR_RED);
            continue;
        }
        if (oring_fault_latched(ch))
        {
            buf[4] = 'F'; buf[5] = 'L'; buf[6] = 'T';
            label_set(&path_lb[ch], buf, COLOR_RED);
            continue;
        }
        if (!on && oring_wants_on(ch))
        {
            // Non-latched trip, waiting for auto-recovery
            buf[4] = 'A'; buf[5] = 'R'; buf[6] = 'M';
            label_set(&path_lb[ch], buf, COLOR_YELLOW);
            continue;
        }
        buf[4] = 'O'; buf[5] = on ? 'N' : 'F'; buf[6] = on ? ' ' : (leaky ? '*' : 'F');
        label_set(&path_lb[ch], buf, on ? COLOR_GREEN : (leaky ? COLOR_ORANGE : COLOR_GRAY));
    }
}

bool ui_double_was_all_on(void)
{
    return double_all_on;
}

void ui_menu_frame(const char *title, uint16_t cursor_y)
{
    gfx_fill_screen(COLOR_BLACK);
    ui_draw_title(title);
    gfx_draw_string(8, 224, &font_8x16, "TAP next  HOLD select", COLOR_GRAY, COLOR_BLACK);
    gfx_fill_rect(0, cursor_y, LCD_WIDTH, 24, COLOR_NAVY);
}

static void menu_row_bg(label_t *lb, uint16_t y, uint16_t bg)
{
    lb->bg = bg;
    gfx_fill_rect(0, y, LCD_WIDTH, 24, bg);
    label_invalidate(lb);
}

uint8_t ui_menu_next(label_t *items, const uint16_t *ys, uint8_t n, uint8_t cursor)
{
    menu_row_bg(&items[cursor], ys[cursor], COLOR_BLACK);
    cursor = (uint8_t)((cursor + 1) % n);
    menu_row_bg(&items[cursor], ys[cursor], COLOR_NAVY);
    return cursor;
}

void ui_all_mos_on(void)
{
    char buf[40];
    const char *deny[CH_COUNT] = {NULL, NULL};
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        oring_deny_t d = oring_request(ch, true);
        if (d != ORING_OK) deny[ch] = oring_deny_str(d);
    }
    if (!deny[CH1] && !deny[CH2])
    {
        ui_toast("ALL MOS ON", COLOR_GREEN, 1500);
        return;
    }
    ch_id_t ch = deny[CH1] ? CH1 : CH2;
    snprintf(buf, sizeof(buf), "CH%d: %s", ch + 1, deny[ch]);
    ui_toast(buf, COLOR_RED, 2500);
}

void ui_toast(const char *msg, uint16_t color, uint32_t ms)
{
    toast_lb = (label_t){.x = 0, .y = TOAST_Y, .font = &font_8x16, .width = 40, .bg = COLOR_BLACK};
    label_set(&toast_lb, msg, color);
    toast_until = millis() + ms;
    toast_active = true;
}

void ui_goto(ui_page_id_t page)
{
    if (page >= PAGE_COUNT) return;
    current = page;
    // Double-click all-off / triple-click all-on only on inspection pages (home, trend, diagnostics); on menu pages consecutive short presses move the cursor, so a short press must fire immediately
    key_set_multi_tap(page == PAGE_HOME || page == PAGE_TREND || page == PAGE_CAPS || page == PAGE_STATS);
    need_enter = true;
    title_shown = false;
    need_draw = true;
}

void ui_init(void)
{
    pages[PAGE_HOME] = &page_home;
    pages[PAGE_TREND] = &page_trend;
    pages[PAGE_CAPS] = &page_caps;
    pages[PAGE_MOS] = &page_mos;
    pages[PAGE_VOLT] = &page_volt;
    pages[PAGE_SETUP] = &page_setup;
    pages[PAGE_STATS] = &page_stats;
    pages[PAGE_MENU] = &page_menu;
    pages[PAGE_BAL] = &page_bal;
    ui_goto(PAGE_HOME);
}

void ui_process(uint32_t now_ms)
{
    key_evt_t evt;
    while ((evt = key_get_event()) != KEY_EVT_NONE)
    {
        LOG("KEY %s page=%d", key_evt_str(evt), current);

        if (evt == KEY_EVT_DOUBLE)
        {
            double_all_on = oring_path_is_enabled(CH1) && oring_path_is_enabled(CH2);
            oring_all_off("user double tap");
            ui_toast("ALL MOS OFF (double tap)", COLOR_RED, 1500);
            continue;
        }
        if (pages[current]->on_key) pages[current]->on_key(evt);
    }

    if (need_enter)
    {
        need_enter = false;
        if (pages[current]->enter) pages[current]->enter();
    }
    if (need_draw)
    {
        need_draw = false;
        toast_active = false;
        pages[current]->draw();
    }

    pages[current]->update(now_ms);
    if (title_shown) update_title();

    // After a hint expires the whole page is redrawn to restore the covered footer
    if (toast_active && (int32_t)(now_ms - toast_until) >= 0)
    {
        toast_active = false;
        need_draw = true;
    }
}
