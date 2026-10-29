#pragma once

// Debug command mailbox: the PC writes RAM via WCH-Link (tools/v203ctl.py); the main loop executes it and writes back the result.
// There is no free UART on the board; this is how the switches can be operated remotely without hardware changes (for testing).
//
// Host steps: write arg -> write req = DBG_REQ_SIG | seq << 8 | cmd -> poll ack; done when (ack >> 8) == seq,
// ack & 0xFF is the result (oring_deny_t / whether current sharing was entered immediately / bitmap of channels accepted for voltage; 0xFF = unknown command).
// Writes whose upper 16 bits of req are not DBG_REQ_SIG are always ignored (prevents SWD bit errors from triggering actions).

#include <stdint.h>

#define DBG_CMD_MAGIC       0x444D4344u     // "DCMD"
#define DBG_REQ_SIG         0xC0DE0000u

typedef enum dbg_cmd_code_t
{
    DBG_CMD_PATH = 1,       // arg: bit0 = channel (0 CH1 / 1 CH2), bit8 = 1 on / 0 off
    DBG_CMD_CLEAR_FAULT,    // arg: channel
    DBG_CMD_ALL_OFF,
    DBG_CMD_REPLUG,         // arg: channel (software replug, X035 2.2+)
    DBG_CMD_SHARE,          // arg: 0 = off, 1..3 = NORMAL / MANUAL / FOLLOW
    DBG_CMD_VOLTAGE,        // arg: low 16 bits mV, bit16..17 = channel bitmap (same as app_set_voltage)
    DBG_CMD_SYSTEM_RESET,   // arg must be DBG_RESET_KEY: immediate software reset (for verifying warm-reset recovery; no ack, dbg_cmd is re-initialized after reset)
    DBG_CMD_LEARN,          // arg: 0 = abort learning, 1 = start, 2 = fit and save (result share_fit_t)
} dbg_cmd_code_t;

#define DBG_RESET_KEY       0x21545352u     // "RST!"

typedef struct dbg_cmd_t
{
    uint32_t magic;
    uint32_t arg;
    uint32_t req;           // cleared after execution
    uint32_t ack;           // seq << 8 | result
    uint32_t count;         // number of commands executed
} dbg_cmd_t;

extern volatile dbg_cmd_t dbg_cmd;

void dbg_cmd_process(uint32_t now_ms);
