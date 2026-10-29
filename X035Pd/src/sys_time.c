#include "sys_time.h"

#include "ch32x035.h"

#define TICKS_PER_US    48u
#define TICKS_PER_MS    48000u

// The 64-bit counter is read in two halves: accepted only if the high word is the same before and after
static uint64_t ticks(void)
{
    volatile uint32_t *cnt = (volatile uint32_t *)&SysTick->CNT;
    uint32_t hi, lo;
    do
    {
        hi = cnt[1];
        lo = cnt[0];
    } while (hi != cnt[1]);
    return ((uint64_t)hi << 32) | lo;
}

void sys_time_init(void)
{
    SysTick->CTLR = 0;
    SysTick->SR = 0;
    SysTick->CNT = 0;
    SysTick->CMP = 0xFFFFFFFFFFFFFFFFull;
    // STE = 1 (start), STCLK = 1 (HCLK), count up, no auto-reload, no interrupt
    SysTick->CTLR = (1u << 0) | (1u << 2);
}

uint32_t sys_time_ms(void)
{
    return (uint32_t)(ticks() / TICKS_PER_MS);
}

uint32_t sys_time_us(void)
{
    return (uint32_t)(ticks() / TICKS_PER_US);
}

void sys_delay_us(uint32_t us)
{
    uint32_t start = (uint32_t)ticks();
    uint32_t span = us * TICKS_PER_US;
    while ((uint32_t)ticks() - start < span) {}
}
