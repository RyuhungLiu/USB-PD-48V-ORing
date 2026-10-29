#pragma once

#include <stdint.h>

// SysTick Control / Status Register Definitions
#define SysTick_CTLR_STRE_Msk  (1UL << 3U)  // auto-reload after counting to the compare value
#define SysTick_CTLR_STCLK_Msk (1UL << 2U)  // clock source (1: HCLK, 0: HCLK/8)
#define SysTick_CTLR_STIE_Msk  (1UL << 1U)  // interrupt enable
#define SysTick_CTLR_STE_Msk   (1UL << 0U)  // counter enable

void delay_init(void);

void delay_us(uint32_t us);
void delay_ms(uint32_t ms);

uint32_t micros(void);
uint32_t millis(void);
