#include "resetlog.h"
#include <stddef.h>
#include <ch32v20x.h>

// RAM 20KB: 0x20000000..0x20004FFF, the stack takes the last 2KB (from 0x20004800).
// The record sits below the stack; .bss ends around 0x20002800 and startup code only zeroes .bss, so it never touches this area
#define RL_ADDR     0x200047C0u
#define RL_MAGIC    0x324C4F47u     // "GOL2": changed in v0.0.103 to invalidate old records that may hold a misidentified board type (LM74700)

typedef struct rl_rec_t
{
    uint32_t magic;
    uint32_t stage;
    uint32_t ms;
    uint32_t resets;
    uint32_t check;                 // = ~magic ^ resets, prevents random power-on values from being mistaken for a record
    uint32_t paths;                 // bit0/1 = CH1/CH2 conducting, bit8..15 = board type (for immediate restore after warm reset)
    uint16_t ref_mv[2];
    uint32_t paths_check;           // = ~magic ^ paths ^ ref_mv[0] ^ (ref_mv[1] << 16)
    uint32_t latch;                 // bit0/1 = CH1/CH2 latched
    char     reason[2][RL_REASON_LEN];
    uint32_t latch_check;           // = ~magic ^ latch ^ character-by-character sum of the reason text
} rl_rec_t;

#define REC ((volatile rl_rec_t *)RL_ADDR)

extern char _ebss[];
_Static_assert(sizeof(rl_rec_t) <= 64, "resetlog record too large");

static rl_prev_t prev;
static bool      enabled;
static uint8_t   prev_paths;
static uint16_t  prev_ref[2];
static uint8_t   prev_latch;
static char      prev_reason[2][RL_REASON_LEN];

static uint32_t latch_check(void)
{
    uint32_t c = ~RL_MAGIC ^ REC->latch;
    for (uint8_t ch = 0; ch < 2; ch++)
        for (uint8_t i = 0; i < RL_REASON_LEN; i++) c = (c << 5 | c >> 27) ^ (uint8_t)REC->reason[ch][i];
    return c;
}

static uint32_t paths_check(uint32_t paths, uint16_t r0, uint16_t r1)
{
    return ~RL_MAGIC ^ paths ^ r0 ^ ((uint32_t)r1 << 16);
}

void resetlog_init(reset_cause_t cause)
{
    prev.cause = cause;

    // If .bss grows into the record area, do not use it (the link address cannot be checked at compile time)
    if ((uintptr_t)_ebss > RL_ADDR) return;
    enabled = true;

    bool valid = REC->magic == RL_MAGIC && REC->check == (~RL_MAGIC ^ REC->resets);
    if (valid && cause != RESET_CAUSE_POWER_ON)
    {
        prev.valid = true;
        prev.stage = (uint8_t)REC->stage;
        prev.ms = REC->ms;
        prev.resets = REC->resets + 1;
        if (REC->paths_check == paths_check(REC->paths, REC->ref_mv[0], REC->ref_mv[1]))
        {
            prev_paths = (uint8_t)(REC->paths & 0x03);
            prev_ref[0] = REC->ref_mv[0];
            prev_ref[1] = REC->ref_mv[1];
        }
        if (REC->latch_check == latch_check())
        {
            prev_latch = (uint8_t)(REC->latch & 0x03);
            for (uint8_t ch = 0; ch < 2; ch++)
                for (uint8_t i = 0; i < RL_REASON_LEN; i++) prev_reason[ch][i] = REC->reason[ch][i];
            prev_reason[0][RL_REASON_LEN - 1] = prev_reason[1][RL_REASON_LEN - 1] = 0;
        }
    }

    REC->magic = RL_MAGIC;
    REC->resets = prev.resets;
    REC->check = ~RL_MAGIC ^ prev.resets;
    REC->stage = RL_STAGE_BOOT;
    REC->ms = 0;
    REC->paths = 0;
    REC->ref_mv[0] = REC->ref_mv[1] = 0;
    REC->paths_check = paths_check(0, 0, 0);
    REC->latch = 0;
    for (uint8_t ch = 0; ch < 2; ch++)
        for (uint8_t i = 0; i < RL_REASON_LEN; i++) REC->reason[ch][i] = 0;
    REC->latch_check = latch_check();
}

void resetlog_save_paths(uint8_t mask, uint8_t hw, const uint16_t ref_mv[2])
{
    if (!enabled) return;
    uint32_t paths = mask | ((uint32_t)hw << 8);
    REC->paths = paths;
    REC->ref_mv[0] = ref_mv[0];
    REC->ref_mv[1] = ref_mv[1];
    REC->paths_check = paths_check(paths, ref_mv[0], ref_mv[1]);
}

bool resetlog_early_paths(uint32_t min_uptime_ms, uint8_t *mask, uint8_t *hw)
{
    // Reset flags are cleared by sys_init after reading; at this point they are still the cause of this reset
    if (RCC_GetFlagStatus(RCC_FLAG_PORRST) != RESET || RCC_GetFlagStatus(RCC_FLAG_LPWRRST) != RESET) return false;
    if (RCC_GetFlagStatus(RCC_FLAG_IWDGRST) == RESET && RCC_GetFlagStatus(RCC_FLAG_SFTRST) == RESET &&
        RCC_GetFlagStatus(RCC_FLAG_PINRST) == RESET)
        return false;
    if (REC->magic != RL_MAGIC || REC->check != (~RL_MAGIC ^ REC->resets) || REC->ms < min_uptime_ms) return false;
    if (REC->paths_check != paths_check(REC->paths, REC->ref_mv[0], REC->ref_mv[1])) return false;
    *mask = (uint8_t)(REC->paths & 0x03);
    *hw = (uint8_t)(REC->paths >> 8);
    return true;
}

void resetlog_save_latch(uint8_t mask, const char *const reason[2])
{
    if (!enabled) return;
    REC->latch = mask;
    for (uint8_t ch = 0; ch < 2; ch++)
    {
        const char *s = (mask & (1u << ch)) ? reason[ch] : NULL;
        uint8_t i = 0;
        for (; s && s[i] && i < RL_REASON_LEN - 1; i++) REC->reason[ch][i] = s[i];
        for (; i < RL_REASON_LEN; i++) REC->reason[ch][i] = 0;
    }
    REC->latch_check = latch_check();
}

bool resetlog_warm_latch(uint8_t *mask, char reason[2][RL_REASON_LEN])
{
    bool warm = prev.cause == RESET_CAUSE_IWDG || prev.cause == RESET_CAUSE_SOFTWARE ||
                prev.cause == RESET_CAUSE_PIN;
    if (!prev.valid || !warm || !prev_latch) return false;
    *mask = prev_latch;
    for (uint8_t ch = 0; ch < 2; ch++)
        for (uint8_t i = 0; i < RL_REASON_LEN; i++) reason[ch][i] = prev_reason[ch][i];
    return true;
}

bool resetlog_warm_paths(uint32_t min_uptime_ms, uint8_t *mask, uint16_t ref_mv[2])
{
    bool warm = prev.cause == RESET_CAUSE_IWDG || prev.cause == RESET_CAUSE_SOFTWARE ||
                prev.cause == RESET_CAUSE_PIN;
    if (!prev.valid || !warm || prev.ms < min_uptime_ms || !prev_paths) return false;
    *mask = prev_paths;
    ref_mv[0] = prev_ref[0];
    ref_mv[1] = prev_ref[1];
    return true;
}

void resetlog_mark(rl_stage_t stage, uint32_t now_ms)
{
    if (!enabled) return;
    REC->stage = stage;
    REC->ms = now_ms;
}

const rl_prev_t *resetlog_prev(void)
{
    return &prev;
}

const char *resetlog_stage_str(uint8_t stage)
{
    switch (stage)
    {
        case RL_STAGE_BOOT:  return "BOOT";
        case RL_STAGE_PD:    return "PD";
        case RL_STAGE_KEY:   return "KEY";
        case RL_STAGE_METER: return "METER";
        case RL_STAGE_ORING: return "ORING";
        case RL_STAGE_APP:   return "APP";
        case RL_STAGE_UI:    return "UI";
        case RL_STAGE_SLOW:  return "SLOW";
        case RL_STAGE_FLASH: return "FLASH";
        case RL_STAGE_IDLE:  return "IDLE";
        default:             return "?";
    }
}
