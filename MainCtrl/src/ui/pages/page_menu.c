#include "page_menu.h"

#include <stdio.h>
#include "config.h"
#include "bsp/delay.h"
#include "drivers/i2c_bus.h"
#include "pd/pd_node.h"
#include "power/meter.h"
#include "ui/gfx.h"
#include "ui/widget.h"

// Main menu: entered by long press from the inspection pages (HOME / TREND / CAPS / STATISTICS); bottom two rows show system info (replaces the former DIAG page)
// Layout (320x240)
//  y=0    title bar
//  y=30   > OUTPUT                        16x24, cursor row dark purple background
//  y=58   > VOLTAGE
//  y=86   > BALANCE
//  y=114  > SETUP
//  y=142  > BACK
//  y=172  FW 0.0.60  X035 2.0.0 / 2.0.0           8x16 system info
//  y=188  INA OK/OK  I2C 0  UP 01:23:45
//  y=204  description of the selected item
//  y=224  TAP next  HOLD select

typedef enum item_t
{
    ITEM_OUTPUT = 0,
    ITEM_VOLTAGE,
    ITEM_BALANCE,
    ITEM_SETUP,
    ITEM_BACK,
    ITEM_COUNT,
} item_t;

static const uint16_t item_y[ITEM_COUNT] = {30, 58, 86, 114, 142};
static const char *const item_name[ITEM_COUNT] = {"OUTPUT", "VOLTAGE", "BALANCE", "SETUP", "BACK"};
static const char *const item_desc[ITEM_COUNT] = {
    "MOS on / off, fault clear",
    "PD target voltage per channel",
    "current sharing mode",
    "auto on, OCP limit, energy, stats reset",
    "back to view pages",
};
static const ui_page_id_t item_page[ITEM_COUNT] = {PAGE_MOS, PAGE_VOLT, PAGE_BAL, PAGE_SETUP, PAGE_HOME};

static uint8_t cursor;
static label_t item_lb[ITEM_COUNT];
static label_t hint_lb, sys_lb[2];

static void draw(void)
{
    ui_menu_frame("MENU", item_y[cursor]);
    for (uint8_t i = 0; i < ITEM_COUNT; i++)
        item_lb[i] = (label_t){.x = 8, .y = item_y[i], .font = &font_16x24, .width = 19,
                               .bg = (i == cursor) ? COLOR_NAVY : COLOR_BLACK};
    hint_lb = (label_t){.x = 8, .y = 204, .font = &font_8x16, .width = 38, .bg = COLOR_BLACK};
    for (uint8_t i = 0; i < 2; i++)
        sys_lb[i] = (label_t){.x = 8, .y = (uint16_t)(172 + 16 * i), .font = &font_8x16, .width = 38, .bg = COLOR_BLACK};
}

static void update(uint32_t now_ms)
{
    (void)now_ms;
    char buf[24];
    for (uint8_t i = 0; i < ITEM_COUNT; i++)
    {
        snprintf(buf, sizeof(buf), "%c %s", cursor == i ? '>' : ' ', item_name[i]);
        label_set(&item_lb[i], buf, i == ITEM_BACK ? COLOR_WHITE : COLOR_CYAN);
    }
    label_set(&hint_lb, item_desc[cursor], COLOR_GRAY);

    // System info: firmware versions of the main controller / both X035s; current sensing, I2C errors, run time
    char l[48], v[2][12];
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        const pd_node_t *n = pd_node_get(ch);
        if (n->version_valid) snprintf(v[ch], sizeof(v[ch]), "%u.%u.%u", n->fw_major, n->fw_minor, n->fw_patch);
        else snprintf(v[ch], sizeof(v[ch]), "--");
    }
    snprintf(l, sizeof(l), "FW %u.%u.%u  X035 %s / %s", CONFIG_FW_VERSION_MAJOR, CONFIG_FW_VERSION_MINOR,
             CONFIG_FW_VERSION_PATCH, v[0], v[1]);
    label_set(&sys_lb[0], l, COLOR_DARKGRAY);
    uint32_t sec = millis() / 1000;
    bool ina_ok = meter_get(CH1)->online && meter_get(CH2)->online;
    snprintf(l, sizeof(l), "INA %s/%s  I2C %lu  UP %02lu:%02lu:%02lu", meter_get(CH1)->online ? "OK" : "--",
             meter_get(CH2)->online ? "OK" : "--", i2c_bus_error_count(), sec / 3600, (sec / 60) % 60, sec % 60);
    label_set(&sys_lb[1], l, ina_ok ? COLOR_DARKGRAY : COLOR_RED);
}

static void on_key(key_evt_t evt)
{
    if (evt == KEY_EVT_CLICK)
    {
        cursor = ui_menu_next(item_lb, item_y, ITEM_COUNT, cursor);
    }
    else if (evt == KEY_EVT_LONG)
    {
        ui_page_id_t p = item_page[cursor];
        if (cursor == ITEM_BACK) cursor = 0;
        ui_goto(p);
    }
}

const ui_page_t page_menu = {draw, update, on_key, NULL};
