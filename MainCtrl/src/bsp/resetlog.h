#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bsp/sys.h"

// Reset tracking: the main loop records the current execution stage and time at a fixed RAM address. That address is outside .bss,
// so its content survives a software/watchdog reset (invalid after power loss, detected by magic); reading it at boot shows where the last run got stuck

typedef enum rl_stage_t
{
    RL_STAGE_BOOT = 1,
    RL_STAGE_PD,
    RL_STAGE_KEY,
    RL_STAGE_METER,
    RL_STAGE_ORING,
    RL_STAGE_APP,
    RL_STAGE_UI,
    RL_STAGE_SLOW,
    RL_STAGE_FLASH,
    RL_STAGE_IDLE,
} rl_stage_t;

typedef struct rl_prev_t
{
    bool          valid;        // the last run left a valid record (false after power loss)
    reset_cause_t cause;        // cause of this reset
    uint8_t       stage;        // last stage entered in the previous run
    uint32_t      ms;           // run time when that stage was entered in the previous run
    uint32_t      resets;       // number of resets accumulated without power loss
} rl_prev_t;

// Call after sys_init and before other modules: read the previous record and start a new one
void resetlog_init(reset_cause_t cause);

void resetlog_mark(rl_stage_t stage, uint32_t now_ms);

const rl_prev_t *resetlog_prev(void);

// Record both channels' conduction state, board type (ORing hardware version) and voltage reference (called by ORing on every switch), for immediate restore after warm reset
void resetlog_save_paths(uint8_t mask, uint8_t hw, const uint16_t ref_mv[2]);

// Called at the earliest stage after reset (before sys_init and before resetlog_init): reads the reset flags and the record directly.
// On a warm reset (watchdog / software / reset pin) with a valid record and a previous run of at least min_uptime_ms, returns the channels that were conducting before reset and the board type
bool resetlog_early_paths(uint32_t min_uptime_ms, uint8_t *mask, uint8_t *hw);

// Conduction state left by the previous run: returns true only on a warm reset (watchdog / software / reset pin) with a valid record,
// and a previous run of at least min_uptime_ms (avoids repeatedly turning on in a reset-at-boot loop)
bool resetlog_warm_paths(uint32_t min_uptime_ms, uint8_t *mask, uint16_t ref_mv[2]);

#define RL_REASON_LEN   12              // latch reason text (including the terminating 0)

// Record both channels' latch state and reason (called by ORing on every latch / clear); restored after warm reset so a faulted channel is not turned back on automatically
void resetlog_save_latch(uint8_t mask, const char *const reason[2]);

// Latch left by the previous run: returns true only on a warm reset with a valid record (no run-time requirement)
bool resetlog_warm_latch(uint8_t *mask, char reason[2][RL_REASON_LEN]);
const char *resetlog_stage_str(uint8_t stage);
