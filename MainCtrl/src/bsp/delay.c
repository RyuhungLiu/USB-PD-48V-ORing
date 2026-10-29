#include "delay.h"

#include <ch32v20x.h>

// SysTick 64-bit free-running up counter (HCLK), no interrupt; at 144MHz it takes about 4000 years to overflow
static uint8_t initialized = 0;

static uint32_t us_divider;
static uint32_t ms_divider;

void delay_init(void)
{
    if (initialized) return;

    SysTick->CTLR = 0;
    SysTick->SR = 0;
    SysTick->CNT = 0;
    SysTick->CMP = 0xFFFFFFFFFFFFFFFFULL;

    us_divider = SystemCoreClock / 1000000;
    ms_divider = SystemCoreClock / 1000;

    SysTick->CTLR = SysTick_CTLR_STE_Msk | SysTick_CTLR_STCLK_Msk;

    initialized = 1;
}

void delay_us(uint32_t us)
{
    if (us == 0) return;

    uint64_t target_count = SysTick->CNT + (uint64_t)us * us_divider;

    while (SysTick->CNT < target_count);
}

void delay_ms(uint32_t ms)
{
    if (ms == 0) return;

    uint64_t target_count = SysTick->CNT + (uint64_t)ms * ms_divider;

    while (SysTick->CNT < target_count);
}

// 64-bit count / 32-bit divisor, returns only the low 32 bits of the quotient (millis/micros wrap at 32 bits anyway).
// Long division 8 bits at a time using only the hardware 32-bit divide, avoiding linking libgcc's __udivdi3 (about 0.9KB);
// remainder < d <= 144000 < 2^18, so shifting left by 8 bits still fits in 32 bits
static uint32_t div64_low(uint64_t n, uint32_t d)
{
    uint32_t q = 0, r = 0;
    for (int8_t shift = 56; shift >= 0; shift -= 8)
    {
        r = (r << 8) | (uint32_t)((n >> shift) & 0xFF);
        q = (q << 8) | (r / d);
        r %= d;
    }
    return q;
}

uint32_t micros(void)
{
    return div64_low(SysTick->CNT, us_divider);
}

uint32_t millis(void)
{
    return div64_low(SysTick->CNT, ms_divider);
}
