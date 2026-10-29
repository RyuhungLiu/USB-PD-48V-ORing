#pragma once

#define CONFIG_FW_VERSION_MAJOR 0
#define CONFIG_FW_VERSION_MINOR 0
#define CONFIG_FW_VERSION_PATCH 103

// Watchdog timeout (ms): LSI ≈ 40kHz, /32 prescaler -> 1.25kHz
#define IWDG_TIMEOUT_MS         500

// Main-loop time slices (ms), corresponding to docs/FLOW.md §3
#define TASK_KEY_PERIOD_MS      5
#define TASK_METER_PERIOD_MS    10
#define TASK_UI_PERIOD_MS       50
#define TASK_SLOW_PERIOD_MS     1000

// ORing protection (docs/FLOW.md §6), evaluated once every TASK_METER_PERIOD_MS
// Turning on is forbidden when there is no current measurement (INA offline); set to 0 to restore the first version's manual degraded mode (no protection)
#define PROT_REQUIRE_ISENSE         1

// Overcurrent: threshold = contract current × (100 + MARGIN)%, capped at MAX; without a contract, use the Type-C default 3A
#define PROT_OCP_MARGIN_PCT         10      // Default margin; the settings page can select 110% / 120% / 130% / 140% (settings ocp_level 0..3)
#define PROT_OCP_NO_CONTRACT_MA     3000
#define PROT_OCP_HELD_MS            1500    // Time to keep using the old contract current after the contract becomes invalid (Soft_Reset renegotiation takes roughly a few hundred ms)
#define PROT_OCP_MAX_MA             11000
#define PROT_OCP_DELAY_MS           50
// For testing: when non-zero, the overcurrent threshold is min(normal threshold, this value), so that the charger's own protection does not act first; set back to 0 for normal use
#define PROT_OCP_TEST_MA            0
// Short circuit: shut off as soon as a single sample exceeds it
#define PROT_SCP_MA                 13000
// Reverse current (ideal diode failure)
#define PROT_REVERSE_MA             (-500)
#define PROT_REVERSE_DELAY_MS       50
// Overtemperature: INA chip temperature
#define PROT_OTP_MC                 85000
#define PROT_OTP_DELAY_MS           100

// Voltage window: the reference is the contract voltage (without a contract, the measured value at the moment of turning on)
//   upper limit = reference × (100 + HIGH)% + MARGIN, lower limit = reference × (100 − LOW)% − MARGIN (lower limit includes line loss)
#define PROT_VWIN_HIGH_PCT          5
#define PROT_VWIN_LOW_PCT           10
#define PROT_VWIN_MARGIN_MV         250
#define PROT_VWIN_DELAY_MS          200
// Shut off whenever this is exceeded, under any circumstances (EPR 48V + 8%)
#define PROT_OVP_ABS_MV             52000
// Minimum input voltage required to turn on
#define PROT_VIN_MIN_MV             4000

// Shut off when INA reads keep failing for longer than this time
#define PROT_ISENSE_LOST_MS         30

// Automatic recovery after a non-latching shutoff (voltage, unplug, Hard Reset, disconnect): conditions must hold for RESTORE_MS,
// and the contract voltage must match the one when the user turned it on; MAX_TRIPS shutoffs within WINDOW_MS turn it into a latch
#define PROT_RESTORE_MS             500
// Input drop: while conducting, if the measured input stays below this fraction of the reference for PROT_VIN_LOST_MS, shut off (without waiting for the voltage-window delay); not latched.
// Each INA conversion takes about 10.6ms and is read every 10ms, so 25ms covers at least two independent conversions: before v0.0.84 a single reading shut it off,
// on 2026-10-21 while plugging/unplugging CH1, CH2 read a single 20.9V (both the PD contract and X035 VBUS were normal) and was falsely shut off
#define PROT_VIN_LOST_PCT           70
#define PROT_VIN_LOST_MS            25
// Load-transfer overcurrent (OCP XFER): at OCP, if the other channel is not supplying (path off / current below the value below / just shut off),
// it is treated as a load transfer caused by the other channel dropping out, not latched; recover after the other channel turns on again, or retry after RETRY (still counted toward PROT_TRIP_MAX)
#define PROT_XFER_PARTNER_MA        200
// Software replug (X035 2.2+ ErrorRecovery): time the CC termination is removed; the other channel's input must be above this voltage to do it (3V3 comes only from the two VINs)
#define PROT_REPLUG_MS              500
#define PROT_REPLUG_PARTNER_MV      4500
// Warm reset restores conduction immediately: only if the previous run lasted at least this long (so a reset-at-boot loop does not keep turning it on);
// after restoring, for at most this long before the PD link's first report, only measurement-based protections are applied
#define PROT_WARM_MIN_UPTIME_MS     5000
#define PROT_WARM_HOLD_MS           3000
// Off leakage (LTC4359 board only): current still flowing while off = one of the back-to-back MOSFETs is shorted, latch
#define PROT_OFF_LEAK_MA            150
#define PROT_OFF_LEAK_MS            300
#define PROT_OFF_LEAK_LOW_MA        20      // Low threshold: |I| exceeding this value while off for PROT_OFF_LEAK_LOW_MS is also judged as leakage
#define PROT_OFF_LEAK_LOW_MS        2000
// Abnormal conduction drop: both channels conducting, total current ≥ MIN_TOTAL, this channel's input is more than DV higher than the other's yet carries less than MIN_PCT%,
// a normal ideal diode would have conducted long ago = this channel's MOSFET is not fully on (conducting through the body diode), latch
#define PROT_DROP_DV_MV             400
#define PROT_DROP_MIN_PCT           10
#define PROT_DROP_MIN_TOTAL_MA      1000
#define PROT_DROP_MS                1000
#define PROT_XFER_RECENT_MS         2000
#define PROT_XFER_RETRY_MS          5000
// OCP during current sharing "transfer first, shut off later": sharing running, both channels conducting and combined current not exceeding the sum of both limits (just unevenly distributed),
// when one channel exceeds the OCP threshold, first notify current sharing to transfer current urgently, and relax the OCP delay to the value below; SCP and genuine total overload are unaffected
#define PROT_OCP_SHARE_DELAY_MS     500
#define SHARE_URGENT_STEP_MAX_MV    300     // Per-step limit for the urgent transfer
// Gain estimate: only used if both channels carry ≥ this fraction of total current before and after the adjustment (excludes the nonlinear region where the ideal diode is cut off)
#define SHARE_K_EST_MIN_PCT         15
#define SHARE_PREDICT_PCT           160     // Prediction: do not adjust if the effect of one step k×dv exceeds this percentage of |err| (hysteresis, prevents quantization oscillation)
#define SHARE_TRANSIENT_PCT         30      // Total current change exceeding this fraction within one evaluation period: load transient, do not adjust or update the gain this time
#define PROT_TRIP_WINDOW_MS         60000
// Voltage-dip record (app/stats.c): while conducting, record the minimum VBUS every 10ms; falling below this fraction of the contract voltage counts as one dip, count and longest duration are recorded
#define STATS_SAG_PCT               95
#define PROT_TRIP_MAX               4       // The 4th non-latching shutoff within the window becomes a latch (i.e. 3 automatic retries)
// Recovery backoff after a non-latching shutoff: minimum wait after the 1st / 2nd / 3rd shutoff respectively (avoids repeated trips within a few seconds while the load has not been removed)
#define PROT_BACKOFF_MS             {1000, 3000, 10000}
#define PROT_EVENT_LOG_LEN          8       // Protection event log (RAM ring buffer, read via WCH-Link)

// Current sharing (power/share.c): switch both channels to PPS/AVS, fine-tune the voltage step by step according to the current difference
#define SHARE_PERIOD_MS             500     // Comparison period
#define SHARE_DIFF_PCT              10      // Adjust only when the current difference exceeds this fraction of the total current
#define SHARE_STEP_MV               20      // Adjustment per step (PPS step 20mV)
#define SHARE_RANGE_MV              1000    // Maximum offset relative to the center voltage (also limited by the protection voltage window)
// Center voltage upward headroom (v0.0.97): when both channels are on an adjustable PDO and the center sits at the adjustable PDO's upper limit (e.g. EPR AVS 48V), move the center down to the limit minus this value,
// so both channels can adjust in both directions: feed-forward / OCP assist can move both channels in opposite directions by half each, achieving twice the correction per contract round. Cost: output is this much lower
#define SHARE_CENTER_HEADROOM_MV    500
#define SHARE_MIN_TOTAL_MA          500     // Do not adjust when total current is below this value
#define SHARE_IDLE_ZERO_MS          10000   // After no load for this long, zero the offsets of the two channels one at a time (the next load starts from an even split)
#define SHARE_SWITCH_TIMEOUT_MS     3000    // Timeout waiting for the new contract when switching into/out of PPS
#define SHARE_ACK_TIMEOUT_MS        1500    // Timeout waiting for the new contract after an adjustment (resend on timeout)
#define SHARE_MAX_FAILS             3       // Limit on consecutive failures; exceeding it exits current sharing
#define SHARE_AUTO_SETTLE_MS        2000    // Auto entry: time the preconditions must be continuously met
#define SHARE_AUTO_RETRY_MS         10000   // Backoff after an auto entry failure
#define SHARE_AUTO_MAX_FAULTS       3       // Limit on consecutive failures; exceeding it stops auto entry
#define SHARE_AUTO_RESET_MS         30000   // Clear the failure count after running stably for this long
// Closed loop: online estimate of gain k (how many mA the error current changes per 1V of offset change), used to predict the effect of each step
//   - Adjust only if one step is predicted to make the error smaller (|err| > k × quantization step / 2), eliminating limit cycles caused by quantization
//   - Step size = |err| / k × gain percentage, rounded to 20mV (PPS) / 100mV (AVS)
//   - After the new contract is confirmed, wait SHARE_SETTLE_MS before using the current for the next decision (the current average has low-pass delay)
// How many mA one channel's current (= error current) changes per 1V of offset-difference change. Since v0.0.102 the closed-loop initial value, MANUAL pre-bias limit and intercept trim share this value
// (formerly closed-loop initial value 5000 and pre-bias used a separate 12000; 2026-10-26 measured at 8A: 100mV offset difference changed the split by 0.9A)
#define SHARE_GAIN_MA_PER_V         9000
#define SHARE_K_INIT_MA_PER_V       SHARE_GAIN_MA_PER_V
#define SHARE_K_MIN_MA_PER_V        1000
#define SHARE_K_MAX_MA_PER_V        40000
#define SHARE_SETTLE_MS             300
#define SHARE_SETTLE_AVS_MS         600     // AVS PDO: wait after the charger changes voltage. v0.0.98 instantaneous tracking measured the current fully redistributed ~100ms after the contract (formerly 1500)
#define SHARE_GAIN_PCT              50      // Normal / follow
#define SHARE_STEP_MAX_MV           100
// Learning (started / saved by a debug command while current sharing runs): after the closed loop converges, record the offset difference of the two channels (CH1 − CH2) binned by total current,
// on save, least-squares fit Δoff = a + k × I_total and write it into settings (bound to the fingerprint of both channels' capability tables and the center voltage)
#define SHARE_LEARN_BIN_MA          500     // Total current bin width
#define SHARE_LEARN_BINS            24      // 0 .. 12A
#define SHARE_LEARN_STABLE_MS       3000    // Record only if at least this long has passed since the last voltage adjustment and the last load transient
#define SHARE_LEARN_MIN_SAMPLES     4       // Minimum number of records per bin (one per SHARE_PERIOD_MS) to take part in the fit
#define SHARE_LEARN_MIN_BINS        3
#define SHARE_LEARN_MIN_SPAN_MA     2000    // The current range taking part in the fit must be at least this wide
// MANUAL feed-forward: set the offset difference in one step from the learned line (move only if the post-quantization gap is ≥ 70% of a step), then fine-tune the intercept with a slow closed loop
// (only in the high-load region, predicted with SHARE_GAIN_MA_PER_V; a reverse correction requires the error to be larger than before the last correction, preventing back-and-forth between two steps)
#define SHARE_MANUAL_V0_TOL_MV      1000    // Do not apply if the center voltage differs from the one at learning time by more than this value (use the normal closed loop instead); after lowering the center for headroom the difference is 500
#define SHARE_TRIM_MS               3000    // Fine-tune one step only after the error stays outside the deadband for this long
#define SHARE_TRIM_MAX_MV           500
// Light-load pre-bias: when total current is below this fraction of the sum of both channels' contract currents, set the offset difference according to that current (not the actual current),
// so that on a sudden load up to near full load it is only about one step off, and the stiffer channel does not overshoot its own protection first.
// v0.0.91 used 90% of the smaller contract (4.5A): 1→7A was OK, 1→9A was estimated to push the stiffer channel to 6–7A; v0.0.95 changed to 75% of the sum (7.5A).
// Cost: when total current is below this value the split is not even (biased toward the other channel), and the favored channel does not exceed SHARE_FF_PRE_MAX_PCT% of its contract.
// The "minimum conduction" measured shrink/expand of v0.0.96–101 was removed (v0.0.102): at light load one step already exceeds the load, so one channel carries it all anyway,
// shrinking only swaps the cut-off channel for the stiffer one (v0.0.100 8A slow: shrunk to 0 at 1A, the next ramp went entirely into CH1, charger self-protection)
#define SHARE_FF_PRE_PCT            75
#define SHARE_FF_PRE_MAX_PCT        90
// Feed-forward waits for the current to settle: if total current changes between two evaluations by more than this fraction and more than the value below, treat it as still ramping and check again after SHARE_FF_RECHECK_MS
// (measured: positioned at 3.6A partway up the load ramp, and had to wait another contract round to reach 7A)
#define SHARE_FF_STABLE_PCT         10
#define SHARE_FF_STABLE_MIN_MA      200
#define SHARE_FF_RECHECK_MS         150
// Follow-PDO mode: distribute current in proportion to the two channels' contract current limits (e.g. 5A:3A), allowing the overall voltage to drop
#define SHARE_FOLLOW_DOWN_MV        1500    // Downward offset limit (also limited by the protection voltage window, reserving 500mV for line loss)
#define SHARE_FOLLOW_DIFF_PCT       4       // Deadband for follow-PDO mode (10% is too wide near full load and would let one channel exceed its contract current)
// Near full load: if this is the reverse of the previous over-limit adjustment and the excess in the state it returns to is not smaller than now, stop (prevents endless back-and-forth between two PDOs)
#define SHARE_OL_MEMORY_MS          10000
// OCP assist / fast transient: the settle wait may be skipped only if at least this interval has passed since the last adjustment was sent (too dense only piles up commands; measured once sending 4 steps within 200ms)
#define SHARE_URGENT_GAP_MS         100     // v0.0.100: contract 30ms + charger starts responding ~50ms, then it can send again (formerly 400 → 200);
                                            // 8A slow measurement: partway up the ramp, one step was sent on a false plateau then blocked by 200ms, CH1 climbed to 6.45A and the charger self-protected
// Overload warning: the combined current of the conducting channels exceeds this fraction of their combined contract currents
#define PROT_OVERLOAD_PCT           97
#define SHARE_LIMIT_PCT             97      // When any channel's current exceeds this fraction of its contract current and is above the target, keep adjusting regardless of the deadband
#define SHARE_AVS_STEP_MV           100     // AVS output voltage effective step is 100mV (RDO unit 25mV, lowest 2 bits must be 0)
// Single-channel dropout hold: when one channel is unplugged / disconnected during operation, the other keeps its current state; after the dropped channel renegotiates, restore directly to the recorded voltage
#define SHARE_HOLD_TIMEOUT_MS       30000   // If not recovered before timeout, exit through the normal flow (the other channel switches back to the fixed PDO)
#define SHARE_RESUME_DELAY_MS       300     // After the dropped channel gets a new contract, wait this long before sending the recorded voltage
// Current-sharing diagnostic history (RAM ring buffer about 2KB, read via WCH-Link to analyze convergence), temporarily disabled
#define SHARE_HIST_ENABLE           1
// Diagnostic trace (v0.0.98): for this period after a load transient, record both channels' instantaneous current into the sharing history every SHARE_TRACE_STEP_MS (action 6),
// to see how long the current takes to redistribute after the charger changes voltage (history 128 entries, 1.5s / 20ms = 75 entries, the next transient overwrites it)
#define SHARE_TRACE_MS              1500
#define SHARE_TRACE_STEP_MS         20
