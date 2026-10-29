#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "board.h"

// ORing path switching and protection (docs/FLOW.md §6)
//   - All off by default at power-up; LMx_EN polarity is auto-detected by hardware version (see board.h)
//   - User turn-on requires: link online, INA online, input voltage normal and matching the contract, no latched fault
//   - After turn-on, checked every 10ms: overcurrent/short circuit/reverse current/overtemperature (latched, user must acknowledge to clear),
//     voltage window/unplug/Hard Reset/contract change/disconnect/INA failure (non-latching, automatically turned on again once conditions recover)
//   - No automatic turn-on after a user turn-off, all-off, or latch

typedef enum oring_hw_t
{
    ORING_HW_UNKNOWN = 0,   // Detection failed: pins left floating (off on both versions), turn-on forbidden
    ORING_HW_LM74700,       // First version: high = off, and after turning off the output is still ≈ VIN − Vth
    ORING_HW_LTC4359,       // Revised version: high = on, off truly disconnects
} oring_hw_t;

typedef enum oring_deny_t
{
    ORING_OK = 0,
    ORING_DENY_LINK,        // X035 link offline
    ORING_DENY_HW,          // Hardware version not identified
    ORING_DENY_FAULT,       // Latched fault not cleared
    ORING_DENY_ISENSE,      // INA offline, no current measurement
    ORING_DENY_NO_VIN,      // Input voltage too low or too high
    ORING_DENY_VIN_MISMATCH,// Measured voltage does not match the PD contract
    ORING_DENY_VSET,        // Voltage switch in progress
    ORING_DENY_NO_PARTNER,  // Software replug: the other channel has no input voltage (doing it would power down the whole board)
    ORING_DENY_FW,          // Software replug: X035 firmware older than 2.2
} oring_deny_t;

// Must be called first: detect the hardware version and set both channels off
void oring_init(void);

// Called at the earliest stage of the reset entry (see bsp/startup_reset.S); restores EN immediately on a warm reset
void oring_reset_hook(void);

// On a warm reset (watchdog / software / reset pin) when the previous run lasted long enough, immediately restore conduction on the paths recorded before the reset,
// avoiding inrush from turning on again after VOUT has discharged. Must be called as early as possible after resetlog_init and delay_init. Returns true if anything was restored
bool oring_warm_start(void);

// Warm reset: restore the latched faults from before the reset (including the reason text), so auto output will not turn that channel on again. Must be called after oring_warm_start
bool oring_restore_latch(void);

oring_hw_t oring_hw(void);
oring_hw_t oring_hw_detected(void);     // Raw result of pin detection (may differ from oring_hw when polarity is forced)
const char *oring_hw_str(oring_hw_t hw);

// On the first hardware version "off" cannot disconnect the output; the UI must show a notice
bool oring_off_is_leaky(void);

// User request to turn on/off; turning off always succeeds, turning on may be refused by an interlock
oring_deny_t oring_request(ch_id_t ch, bool on);
void oring_all_off(const char *reason);

// Auto output at power-up: enter a waiting state with ref_mv as the reference, turn on automatically once the contract voltage matches it and the turn-on conditions are met
void oring_arm(ch_id_t ch, uint16_t ref_mv);

bool oring_path_is_enabled(ch_id_t ch);

// Called every 10ms after meter_sample: interlocks, protection, automatic recovery
void oring_process(uint32_t now_ms);

// Reason for the most recent automatic shutoff (returns NULL if none)
const char *oring_last_trip(ch_id_t ch);

// Measured value and time of the most recent shutoff; unit is "mA"/"mV"/"mC", NULL for event-type shutoffs. Returns false if there is no shutoff record
bool oring_last_trip_value(ch_id_t ch, int32_t *value, const char **unit, uint32_t *ms);

// User wants it on (including the state of being shut off by a non-latching fault and waiting for automatic recovery)
bool oring_wants_on(ch_id_t ch);

// Latched fault: oring_clear_fault must be called before it can be turned on again
bool oring_fault_latched(ch_id_t ch);
// Path shut off by protection (waiting for automatic recovery or already latched); user turn-off does not count
bool oring_path_down(ch_id_t ch);
void oring_clear_fault(ch_id_t ch);

// Software replug: shut off this channel with an uncounted REPLUG and clear the latch, then ask X035 to perform ErrorRecovery;
// paths that were on are restored automatically once the new contract comes back; those that were off / latched stay off
oring_deny_t oring_replug(ch_id_t ch);

// Current overcurrent threshold (mA)
int32_t oring_ocp_limit_ma(ch_id_t ch);

// Overload: the combined current of the conducting channels exceeds PROT_OVERLOAD_PCT% of their combined contract currents (shown as OVERLOAD at the DIFF position on the home page)
bool oring_overload(void);

const char *oring_deny_str(oring_deny_t deny);
