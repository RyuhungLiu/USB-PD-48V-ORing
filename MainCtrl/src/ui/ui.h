#pragma once

#include <stdint.h>
#include "drivers/key.h"
#include "ui/widget.h"

// Page management: single-key interaction (docs/FLOW.md section 7, docs/UI_FLOW.md)
//   Inspection pages (HOME -> TREND 5min -> TREND 30s -> CAPS charger capability table -> STATISTICS): short press cycles, long press enters MENU;
//     double click immediately turns off all MOS (emergency off), triple click on the home page turns all on
//   Menu pages (MENU, OUTPUT, VOLTAGE, BALANCE, SETUP): short press goes to the next item (fires immediately), long press executes,
//     BACK always returns one level up

typedef enum ui_page_id_t
{
    PAGE_HOME = 0,
    PAGE_TREND,
    PAGE_CAPS,
    PAGE_MOS,
    PAGE_VOLT,
    PAGE_SETUP,
    PAGE_STATS,
    PAGE_MENU,
    PAGE_BAL,
    PAGE_COUNT,
} ui_page_id_t;

typedef struct ui_page_t
{
    void (*draw)(void);                 // full-page draw: called on entering the page and when restoring after a hint expires; must not reset page state
    void (*update)(uint32_t now_ms);    // periodic refresh (redraws only changed parts)
    void (*on_key)(key_evt_t evt);      // short / long press (double click is handled centrally by ui)
    void (*enter)(void);                // optional: resets page state when switching in from another page (called before draw)
} ui_page_t;

void ui_init(void);

// Called every TASK_UI_PERIOD_MS: handles key events and refreshes the current page
void ui_process(uint32_t now_ms);

void ui_goto(ui_page_id_t page);

// Page draws the title bar (title on the left + both channels' MOS status on the right)
#define UI_TITLE_H 21
void ui_draw_title(const char *title);

// Temporary one-line hint at the bottom (e.g. a rejection reason), cleared after ms milliseconds
void ui_toast(const char *msg, uint16_t color, uint32_t ms);

// Whether both channels were conducting before the latest double-click all-off (triple click uses this to avoid turning on right after turning off)
bool ui_double_was_all_on(void);

// Turn on all MOS (latched faults are not cleared); the result is shown as a hint
void ui_all_mos_on(void);

// Shared by menu pages: full-page background + title + footer hint; move the cursor to the next item (old row back to black, new row dark purple), returns the new cursor
void ui_menu_frame(const char *title, uint16_t cursor_y);
uint8_t ui_menu_next(label_t *items, const uint16_t *ys, uint8_t n, uint8_t cursor);
