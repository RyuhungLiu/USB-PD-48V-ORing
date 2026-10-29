#pragma once

#include <stdint.h>

typedef enum reset_cause_t
{
    RESET_CAUSE_UNKNOWN = 0,
    RESET_CAUSE_POWER_ON,
    RESET_CAUSE_PIN,
    RESET_CAUSE_SOFTWARE,
    RESET_CAUSE_IWDG,
    RESET_CAUSE_WWDG,
    RESET_CAUSE_LOW_POWER,
} reset_cause_t;

void sys_init(void);

// Reconfigure to 144MHz when the PLL configuration was changed unexpectedly; returns whether a fix was applied this time (see notes in sys.c)
#include <stdbool.h>
bool sys_clock_check_repair(void);
uint32_t sys_clock_repairs(void);
uint32_t sys_clock_last_bad_cfgr0(void);
uint32_t sys_clock_cfgr0(void);
reset_cause_t sys_get_reset_cause(void);
const char *sys_reset_cause_str(reset_cause_t cause);
