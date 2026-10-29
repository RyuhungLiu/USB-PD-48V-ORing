#pragma once

#include "ui/ui.h"

// Voltage settings page: short press moves the cursor; long press enters edit, while editing short press cycles the PDO and long press applies
// Before applying, that channel's MOS is turned off automatically; after the new voltage settles the user must turn it back on manually on the MOS page
// With SYNC ON (default), changing either channel switches both together
extern const ui_page_t page_volt;
