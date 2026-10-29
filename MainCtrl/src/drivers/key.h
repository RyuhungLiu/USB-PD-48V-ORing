#pragma once

#include <stdbool.h>
#include <stdint.h>

// TTP223 single touch key (PB10); by default the module outputs high = touched
// Events: single / double / triple click / long press (long press fires immediately when held past the threshold; release then fires no other event)
// Double click fires immediately on the second release (all-off is not delayed); one more tap within KEY_DOUBLE_MS adds a triple click event

#define KEY_ACTIVE_HIGH     1

#define KEY_DEBOUNCE_MS     20
#define KEY_LONG_MS         800
#define KEY_DOUBLE_MS       300     // a single click must wait this long to confirm it is not a double click

typedef enum key_evt_t
{
    KEY_EVT_NONE = 0,
    KEY_EVT_CLICK,
    KEY_EVT_DOUBLE,
    KEY_EVT_LONG,
    KEY_EVT_TRIPLE,     // immediately follows KEY_EVT_DOUBLE
} key_evt_t;

void key_init(void);

// Multi-click detection (double / triple): when disabled, a single click fires immediately on release without waiting KEY_DOUBLE_MS (used by the menu page)
void key_set_multi_tap(bool on);

// Call once every TASK_KEY_PERIOD_MS
void key_scan(uint32_t now_ms);

// Take one event; returns KEY_EVT_NONE when there is none
key_evt_t key_get_event(void);

const char *key_evt_str(key_evt_t evt);
