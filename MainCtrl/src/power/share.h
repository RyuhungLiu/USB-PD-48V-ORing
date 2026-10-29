#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "board.h"

// Current sharing (docs/FLOW.md §6): switch both channels to a PPS/AVS adjustable PDO and fine-tune each voltage step by step according to the current difference between the two channels
//   - Enable: the two channels in turn request an adjustable PDO at the current contract voltage (X035 PD command 0x0D), and enter running after the new contract is confirmed.
//     PDO selection prefers PPS; a channel with no PPS at that voltage stays on the fixed PDO and is not adjusted (only the other channel is adjusted); AVS is used only if neither channel has PPS
//   - Running: compare currents every SHARE_PERIOD_MS; when the difference exceeds SHARE_DIFF_PCT% of the total current, the channel with the larger current
//     is lowered by SHARE_STEP_MV (or the channel with the smaller current raised); the offset is limited to ±SHARE_RANGE_MV and does not leave the protection voltage window
//   - Single-channel dropout (unplug, Hard Reset, link disconnect): enter HOLD, the other channel is left untouched (not switched back to the fixed PDO), keeping the center voltage,
//     offsets and PDO selection record; after the dropped channel renegotiates, wait SHARE_RESUME_DELAY_MS, send the recorded voltage directly and return to running,
//     without the 2s wait of auto entry or convergence from 0. If not recovered within SHARE_HOLD_TIMEOUT_MS, exit through the normal flow
//   - Disable: the two channels in turn request a return to the fixed PDO (0x0C, preferring an exactly matching fixed PDO)
//   - The on/off switch is saved in settings: once enabled (including after power-up, voltage switching, plugging/unplugging), auto entry happens when the preconditions hold continuously for 2s,
//     on failure back off 10s and retry, give up after 3 consecutive failures until the user enables it again
//   - Requires the X035 local-branch firmware (extended protocol + 0xA1 confirmed), otherwise every voltage adjustment would write X035's Flash
//   - Modes (saved in settings):
//       NORMAL: the two channel currents are equal, adjust 20mV every 500ms, deadband 10%
//       MANUAL feed-forward: set the offset difference of the two channels in one step from the learned line Δoff = a + k × I_total (no need to converge step by step),
//              distribution in proportion to contract currents; when the error stays outside the deadband, fine-tune the intercept with a slow closed loop. Without a matching learning result it behaves like the normal closed loop
//              (since v0.0.86 it replaces the former FAST quick-adjust mode; setting value 1 is kept)
//   - Learning: started while current sharing runs, the external load is changed through several levels; when the closed loop has converged (within deadband, no voltage adjustment for 3s, no load transient), record the offset difference
//           binned by total current in 0.5A bins; on save, fit a line and write it into settings
//       FOLLOW follow-PDO: distribute in proportion to the two channels' contract current limits (e.g. 5A:3A), both channels can be fully loaded at the same time;
//              downward offset relaxed to SHARE_FOLLOW_DOWN_MV, allowing the overall voltage to drop
// Note: the current limit of a PPS PDO is usually lower than the fixed PDO (e.g. 3A vs 5A), so the usable current per channel drops during current sharing

typedef enum share_state_t
{
    SHARE_OFF = 0,
    SHARE_ENTER,        // Switch into the adjustable PDO one channel at a time
    SHARE_RUN,
    SHARE_EXIT,         // Switch back to the fixed PDO one channel at a time
    SHARE_HOLD,         // One channel dropped out: the other keeps its current state; wait for the dropped channel to renegotiate and then restore from the record
} share_state_t;

typedef enum share_mode_t
{
    SHARE_MODE_NORMAL = 0,
    SHARE_MODE_MANUAL,
    SHARE_MODE_FOLLOW,
    SHARE_MODE_COUNT,
} share_mode_t;

void share_init(void);

// Called every TASK_METER_PERIOD_MS after meter_sample / oring_process
void share_process(uint32_t now_ms);

// User turns current sharing on/off (saved to settings, auto entry after power-up). Returns false if the enable preconditions are not met yet,
// the reason is given by share_reason(); the intent is kept and auto entry happens once the preconditions are met
bool share_request(bool on);

// User has enabled it (including waiting for preconditions and failure backoff)
bool share_auto_on(void);
// Consecutive failures reached the limit, auto entry has stopped; the user must enable it again
bool share_auto_gave_up(void);
// Precondition not met during the auto wait (NULL if none)
const char *share_wait_reason(void);

// Stop immediately without switching back to the fixed PDO (the caller will renegotiate afterwards, e.g. voltage switching)
void share_abort(const char *reason);

// OCP "transfer first, shut off later": ch has exceeded the OCP threshold; the next evaluation does not wait for the period / settle time and transfers enough current at once as needed
void share_ocp_assist(ch_id_t ch);

share_state_t share_state(void);
const char *share_state_str(share_state_t state);

// Reason for the most recent refusal or stop (NULL if none)
const char *share_reason(void);

// Current-sharing mode: takes effect immediately (switching while running recomputes the offset range) and is saved
share_mode_t share_mode(void);
void share_set_mode(share_mode_t mode);
const char *share_mode_str(share_mode_t mode);

// Under the current mode, the current that ch should carry out of total current total (half for normal / fast, in proportion to contract current limits for follow)
int32_t share_target_ma(ch_id_t ch, int32_t total);

// Learning: start clears the records and begins; stop discards; save fits and saves (learning ends on success)
typedef enum share_fit_t
{
    SHARE_FIT_OK = 0,
    SHARE_FIT_NOT_LEARNING,
    SHARE_FIT_FEW_BINS,         // Not enough valid current bins (SHARE_LEARN_MIN_BINS)
    SHARE_FIT_NARROW,           // Current range narrower than SHARE_LEARN_MIN_SPAN_MA
    SHARE_FIT_NO_CAPS,          // Capability table unknown, cannot bind to the charger
} share_fit_t;

void share_learn_start(void);
void share_learn_stop(void);
share_fit_t share_learn_save(void);
bool share_learning(void);
// MANUAL and the learning result matches the present charger / center voltage (currently running with feed-forward)
bool share_manual_ff(void);

uint16_t share_center_mv(void);
int16_t share_offset_mv(ch_id_t ch);
// This channel stays on the fixed PDO (no PPS at this voltage), only the other channel is adjusted
bool share_anchor(ch_id_t ch);
uint32_t share_adjust_count(void);
