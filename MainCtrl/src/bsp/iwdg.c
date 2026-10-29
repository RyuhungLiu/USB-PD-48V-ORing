#include "iwdg.h"

#include <ch32v20x.h>

#define IWDG_LSI_HZ       40000U
#define IWDG_PRESCALER    32U
#define IWDG_RELOAD_MAX   0x0FFFU

void iwdg_init(uint16_t timeout_ms)
{
    uint32_t reload = (uint32_t)timeout_ms * (IWDG_LSI_HZ / IWDG_PRESCALER) / 1000U;
    if (reload == 0) reload = 1;
    if (reload > IWDG_RELOAD_MAX) reload = IWDG_RELOAD_MAX;

    // Note: on the V4B core DBGMCU_CR is CSR 0x7C0, writable only in debug mode;
    // calling DBGMCU_Config() in run mode triggers an illegal instruction exception, so the watchdog is not frozen;
    // staying at a breakpoint longer than the timeout causes a reset
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_32);
    IWDG_SetReload((uint16_t)reload);
    IWDG_ReloadCounter();
    IWDG_Enable();
}

void iwdg_feed(void)
{
    IWDG_ReloadCounter();
}
