#include "flash.h"

#include <string.h>
#include <ch32v20x.h>

// System clock 144MHz: conservatively divide HCLK by 2 during erase/write (WCH-Link also lowers the PLL to 120MHz when flashing),
// restored afterwards. During the division SysTick and peripheral clocks are halved; erase/write is very short, so the effect is negligible
static uint32_t slow_down(void)
{
    uint32_t cfgr0 = RCC->CFGR0;
    RCC->CFGR0 = (cfgr0 & ~RCC_HPRE) | RCC_HPRE_DIV2;
    return cfgr0 & RCC_HPRE;
}

static void restore(uint32_t hpre)
{
    RCC->CFGR0 = (RCC->CFGR0 & ~RCC_HPRE) | hpre;
}

bool flash_erase_page(uint32_t addr)
{
    if (addr & (FLASH_PAGE_SIZE - 1)) return false;

    // The SDK fast page erase does not check errors; rely on the full-page verification below
    uint32_t hpre = slow_down();
    FLASH_Unlock_Fast();
    FLASH_ErasePage_Fast(addr);
    FLASH_Lock_Fast();
    FLASH_Lock();
    restore(hpre);

    // The erased value is not necessarily 0xFFFFFFFF (chip dependent); only require the whole page to be uniform
    const volatile uint32_t *p = (const volatile uint32_t *)addr;
    uint32_t blank = p[0];
    for (uint32_t i = 1; i < FLASH_PAGE_SIZE / 4; i++)
    {
        if (p[i] != blank) return false;
    }
    return true;
}

bool flash_write(uint32_t addr, const void *data, uint32_t len)
{
    if ((addr | len) & 3) return false;

    const uint16_t *src = (const uint16_t *)data;
    FLASH_Status st = FLASH_COMPLETE;

    uint32_t hpre = slow_down();
    FLASH_Unlock();
    for (uint32_t i = 0; i < len / 2 && st == FLASH_COMPLETE; i++)
        st = FLASH_ProgramHalfWord(addr + i * 2, src[i]);
    FLASH_Lock();
    restore(hpre);

    return st == FLASH_COMPLETE && memcmp((const void *)addr, data, len) == 0;
}
