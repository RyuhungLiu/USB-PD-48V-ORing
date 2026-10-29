#include "nv.h"

#include <stdbool.h>
#include <string.h>
#include "ch32x035.h"

#define NV_ADDR         0x0800F700u     // last page of the 62KB Flash (the linker script limits the program to 61KB)
#define NV_PAGE_SIZE    256u
#define NV_MAGIC        0x31445058u     // "XPD1"

typedef struct nv_record_t
{
    uint32_t    magic;
    nv_target_t target;
    uint16_t    pad;
    uint32_t    check;                  // = magic ^ each word
} nv_record_t;

static nv_target_t cur;
static bool        valid;

static uint32_t record_check(const nv_record_t *r)
{
    const uint32_t *w = (const uint32_t *)&r->target;
    return r->magic ^ w[0] ^ w[1] ^ 0xA5A5A5A5u;
}

void nv_init(void)
{
    const nv_record_t *r = (const nv_record_t *)NV_ADDR;
    valid = r->magic == NV_MAGIC && r->check == record_check(r);
    if (valid) cur = r->target;
}

const nv_target_t *nv_target(void)
{
    return valid ? &cur : NULL;
}

void nv_save_target(const nv_target_t *t)
{
    if (valid && memcmp(&cur, t, sizeof(cur)) == 0) return;

    nv_record_t r;
    memset(&r, 0, sizeof(r));
    r.magic = NV_MAGIC;
    r.target = *t;
    r.check = record_check(&r);

    uint32_t page[NV_PAGE_SIZE / 4];
    memset(page, 0xFF, sizeof(page));
    memcpy(page, &r, sizeof(r));

    // CPU stalls during erase/write (a few milliseconds); a PD message may miss its GoodCRC and the partner will retransmit
    FLASH_Unlock_Fast();
    FLASH_ErasePage_Fast(NV_ADDR);
    FLASH_BufReset();
    for (uint32_t i = 0; i < NV_PAGE_SIZE / 4; i++) FLASH_BufLoad(NV_ADDR + 4 * i, page[i]);
    FLASH_ProgramPage_Fast(NV_ADDR);
    FLASH_Lock_Fast();

    nv_init();
}
