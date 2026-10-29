#pragma once

#include <stdint.h>

// Time base: SysTick free-runs counting up at HCLK (48MHz) (64-bit, no interrupt used)

void sys_time_init(void);

uint32_t sys_time_ms(void);
uint32_t sys_time_us(void);         // 32-bit wraparound (about 71 minutes), only for short intervals

void sys_delay_us(uint32_t us);
