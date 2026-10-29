#include "sys.h"

#include <stdbool.h>
#include <ch32v20x.h>

static reset_cause_t reset_cause = RESET_CAUSE_UNKNOWN;

// Read and clear the reset flags; when several flags are set, pick the most specific cause by priority
static reset_cause_t read_reset_cause(void)
{
    reset_cause_t cause = RESET_CAUSE_UNKNOWN;

    if (RCC_GetFlagStatus(RCC_FLAG_IWDGRST) != RESET)       cause = RESET_CAUSE_IWDG;
    else if (RCC_GetFlagStatus(RCC_FLAG_WWDGRST) != RESET)  cause = RESET_CAUSE_WWDG;
    else if (RCC_GetFlagStatus(RCC_FLAG_SFTRST) != RESET)   cause = RESET_CAUSE_SOFTWARE;
    else if (RCC_GetFlagStatus(RCC_FLAG_LPWRRST) != RESET)  cause = RESET_CAUSE_LOW_POWER;
    else if (RCC_GetFlagStatus(RCC_FLAG_PORRST) != RESET)   cause = RESET_CAUSE_POWER_ON;
    else if (RCC_GetFlagStatus(RCC_FLAG_PINRST) != RESET)   cause = RESET_CAUSE_PIN;

    RCC_ClearFlag();
    return cause;
}

void sys_init(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();

    reset_cause = read_reset_cause();
}

reset_cause_t sys_get_reset_cause(void)
{
    return reset_cause;
}

// ============================================================================
// Clock guard: measured that every WCH-Link attach (wlink dump/reset/sdi-print enable etc.) changes
// RCC_CFGR0.PLLMUL to x15 (144MHz -> 120MHz), shifting all peripheral clocks by -16%, so the UART loses contact with the X035.
// It does not happen without debugger activity. Checked every 10ms; if changed, switch to HSI, reconfigure PLL x18, switch back, and count it.
// ============================================================================

#define PLL_CFG_MASK      (RCC_PLLSRC | RCC_PLLXTPRE | RCC_PLLMULL)
#define PLL_CFG_EXPECTED  (RCC_PLLMULL18)   // HSI undivided (EXTEN_PLL_HSI_PRE) x 18 = 144MHz

static uint32_t clock_repairs;
static uint32_t clock_last_bad_cfgr0;

static bool wait_bits(volatile uint32_t *reg, uint32_t mask, uint32_t value)
{
    for (uint32_t i = 0; i < 200000; i++)
    {
        if ((*reg & mask) == value) return true;
    }
    return false;
}

bool sys_clock_check_repair(void)
{
    uint32_t cfgr0 = RCC->CFGR0;
    if ((cfgr0 & PLL_CFG_MASK) == PLL_CFG_EXPECTED && (cfgr0 & RCC_SWS) == RCC_SWS_PLL)
        return false;

    clock_last_bad_cfgr0 = cfgr0;

    // 1) switch system clock to HSI; 2) turn off PLL; 3) write x18; 4) turn on PLL and wait for lock; 5) switch back to PLL
    RCC->CFGR0 &= ~RCC_SW;
    wait_bits(&RCC->CFGR0, RCC_SWS, RCC_SWS_HSI);
    RCC->CTLR &= ~RCC_PLLON;
    wait_bits(&RCC->CTLR, RCC_PLLRDY, 0);
    EXTEN->EXTEN_CTR |= EXTEN_PLL_HSI_PRE;
    RCC->CFGR0 = (RCC->CFGR0 & ~PLL_CFG_MASK) | PLL_CFG_EXPECTED;
    RCC->CTLR |= RCC_PLLON;
    wait_bits(&RCC->CTLR, RCC_PLLRDY, RCC_PLLRDY);
    RCC->CFGR0 = (RCC->CFGR0 & ~RCC_SW) | RCC_SW_PLL;
    wait_bits(&RCC->CFGR0, RCC_SWS, RCC_SWS_PLL);

    clock_repairs++;
    return true;
}

uint32_t sys_clock_repairs(void)
{
    return clock_repairs;
}

uint32_t sys_clock_last_bad_cfgr0(void)
{
    return clock_last_bad_cfgr0;
}

uint32_t sys_clock_cfgr0(void)
{
    return RCC->CFGR0;
}

const char *sys_reset_cause_str(reset_cause_t cause)
{
    switch (cause)
    {
        case RESET_CAUSE_POWER_ON:  return "POR";
        case RESET_CAUSE_PIN:       return "PIN";
        case RESET_CAUSE_SOFTWARE:  return "SW";
        case RESET_CAUSE_IWDG:      return "IWDG";
        case RESET_CAUSE_WWDG:      return "WWDG";
        case RESET_CAUSE_LOW_POWER: return "LPWR";
        default:                    return "UNKNOWN";
    }
}
