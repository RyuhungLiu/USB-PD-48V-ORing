#include "share.h"

#include <stddef.h>
#include "config.h"
#include "app/app.h"
#include "app/settings.h"
#include "bsp/log.h"
#include "pd/pd_node.h"
#include "power/meter.h"
#include "power/oring.h"

#define CH_OTHER(ch)    ((ch_id_t)((ch) == CH1 ? CH2 : CH1))
#define MV_MATCH        100     // Allowed deviation between the contract voltage and the requested value

typedef struct share_ctx_t
{
    share_state_t state;
    share_mode_t  mode;
    uint16_t      v0;                   // Center voltage (contract voltage at enable time, aligned to 20mV)
    bool          anchor[CH_COUNT];     // This channel stays on the fixed PDO and is not adjusted (takes precedence over AVS when there is no PPS at this voltage), only the other channel is adjusted
    int16_t       off[CH_COUNT];        // Offset of each channel relative to v0
    ch_id_t       step_ch;              // Channel currently being handled in ENTER/EXIT
    bool          step_sent;
    uint32_t      t_mark;               // Time the command was sent
    uint32_t      seq_mark;             // contract_seq when the command was sent
    bool          adj_pending;          // RUN: adjustment sent, waiting for the new contract
    ch_id_t       adj_ch;
    int16_t       adj_prev;             // Offset before the adjustment, restored on rejection / timeout
    uint32_t      settle_until;         // After the new contract is confirmed, wait for the current to settle
    int8_t        urgent_ch;            // OCP assist: channel that exceeded the OCP threshold, -1 = none
    int32_t       k;                    // Online-estimated gain: mA of error-current change per 1V of offset
    bool          k_learned;            // A measured value exists (the first measurement directly replaces the initial value)
    bool          est_pending;          // Effect of the previous step is pending measurement
    int32_t       est_err0;             // Error before the adjustment (mA)
    int32_t       est_total0;           // Total current before the adjustment
    int32_t       est_min0;             // Current of the smaller channel before the adjustment
    int16_t       est_dv;               // Voltage change of this step in the error-reducing direction (mV, positive)
    int32_t       prev_total;           // Total current at the previous evaluation (transient detection)
    uint32_t      last_adj_ms;
    int16_t       lim_lo[CH_COUNT];     // Lower / upper offset limit per channel: SHARE_RANGE_MV ∩ protection window ∩ that channel's adjustable PDO voltage range
    int16_t       lim_hi[CH_COUNT];
    uint8_t       fails[CH_COUNT];      // Consecutive failure count per channel (cleared only by that channel's success)
    bool          saturated;
    const char   *reason;
    uint32_t      adjustments;

    // Auto entry (saved in settings once the user enables it; enter when conditions are met, back off and retry on failure)
    bool          auto_on;
    bool          auto_gave_up;         // Consecutive failures reached the limit, waiting for the user to enable again
    uint8_t       auto_faults;
    uint32_t      auto_ok_since;        // Start of the period during which the preconditions have been continuously met, 0 = not met
    uint32_t      auto_retry_at;
    uint32_t      run_since;
    uint8_t       lost_mask;            // HOLD: channels that dropped out (bit0 CH1, bit1 CH2)
    ch_id_t       resume_ch;            // HOLD: channel currently being restored
    uint32_t      hold_since;
    uint32_t      back_since;           // HOLD: time the dropped channel got a contract again, 0 = not yet
    const char   *wait_reason;          // Precondition not met during the auto wait

    // Record of over-limit adjustments near full load (placed at the end so diagnostic script offsets stay unchanged)
    int8_t        ol_dir;               // Direction of the last over-limit adjustment: +1 = current moved from CH1 to CH2, -1 the opposite, 0 = none
    int32_t       ol_from_excess;       // Maximum excess before the last over-limit adjustment (mA)
    uint32_t      ol_ms;
    uint32_t      idle_since;           // Start of total current below SHARE_MIN_TOTAL_MA, 0 = loaded
    uint32_t      transient_ms;         // Most recent load transient
    int16_t       trim;                 // MANUAL: correction of the learned line's intercept by the slow closed loop (mV)
    uint32_t      trim_since;           // MANUAL: start of the error being outside the deadband, 0 = not outside
    uint16_t      v_exit;               // Voltage to return to on exit (contract voltage at entry; the center may have been moved down for headroom)
    uint8_t       adj_mask;             // RUN: channels with an adjustment sent and waiting for the new contract (bit0 CH1, bit1 CH2; both may be in flight at once)
    int16_t       adj_prev2[CH_COUNT];  // Offset of each channel before the adjustment, restored on rejection / timeout
    uint32_t      adj_seq[CH_COUNT];    // contract_seq of each channel when sent
    uint32_t      trace_ms;             // Diagnostic trace: time of the previous instantaneous current record
    int32_t       inst_prev;            // Instantaneous total current of the previous 10ms (fast transient detection)
    int32_t       fast_ref;             // Instantaneous total current at the last fast-transient positioning, 0 = average has caught up (prevents repeated positioning from single samples while the average lags)
    int8_t        trim_dir;             // MANUAL: direction of the last intercept trim (+1 / -1, 0 = none)
    int32_t       trim_err0;            // |error| before the last trim (mA)
    int32_t       trim_total0;          // Total current at the last trim
} share_ctx_t;

static share_ctx_t sh;

// Learning records (read via WCH-Link: tools/v203ctl.py learn status). Global and used: LTO does not rename it
typedef struct share_learn_bin_t
{
    int32_t  sum_ma;                    // Accumulated total current
    int32_t  sum_mv;                    // Accumulated offset difference (CH1 − CH2, corrected for residual error)
    uint16_t n;
    uint16_t pad;
} share_learn_bin_t;

typedef struct share_learn_t
{
    uint32_t magic;                     // 0x314E524C "LRN1"
    uint8_t  active;
    uint8_t  result;                    // Result of the most recent save, share_fit_t, 0xFF = not saved
    uint8_t  bins;                      // The following are the currently active learning results (taken from settings at boot)
    uint8_t  valid;
    int16_t  a_mv;
    int16_t  k_mv_per_a;
    uint16_t resid_mv;
    uint16_t v0;
    share_learn_bin_t bin[SHARE_LEARN_BINS];
} share_learn_t;

__attribute__((used)) share_learn_t share_learn = {.magic = 0x314E524C, .result = 0xFF};

// Extra bits for history action (v0.0.94): adjustment sent by the closed loop (not feed-forward) / OCP assist active at the time
#define SHARE_HIST_LOOP     0x10
#define SHARE_HIST_URGENT   0x20
#define SHARE_HIST_FAST     0x40    // Fast transient (based on instantaneous current)

#if SHARE_HIST_ENABLE
// Diagnostic history: during RUN, one entry per comparison period (when total current reaches the threshold), ring buffer overwrite.
// Convergence / oscillation can be analyzed by reading RAM via WCH-Link, without relying on the SDI log
#define SHARE_HIST_LEN  128     // × 500ms ≈ 64s

typedef struct share_hist_t
{
    uint32_t t_ms;
    int16_t  i_ma[CH_COUNT];
    int16_t  off_mv[CH_COUNT];
    uint8_t  action;            // 0 = no adjustment, 1 = adjust CH1, 2 = adjust CH2, 3 = saturated, 4 = load transient, 5 = both channels at once, 6 = instantaneous trace; | SHARE_HIST_LOOP / _URGENT
    uint8_t  pad[3];
} share_hist_t;

typedef struct share_hist_buf_t
{
    uint32_t     magic;         // 0x48495354 "HIST", for locating it in RAM
    uint32_t     count;         // Cumulative number of entries written (next index = count % LEN)
    share_hist_t e[SHARE_HIST_LEN];
} share_hist_buf_t;

static share_hist_buf_t share_hist;

static void hist_add(uint32_t now_ms, int32_t i1, int32_t i2, uint8_t action)
{
    share_hist_t *h = &share_hist.e[share_hist.count % SHARE_HIST_LEN];
    h->t_ms = now_ms;
    h->i_ma[CH1] = (int16_t)i1;
    h->i_ma[CH2] = (int16_t)i2;
    h->off_mv[CH1] = sh.off[CH1];
    h->off_mv[CH2] = sh.off[CH2];
    h->action = action;
    share_hist.count++;
}
#else
#define hist_add(now_ms, i1, i2, action) ((void)0)
#endif

static bool kind_adjustable(pdo_kind_t kind)
{
    return kind == PDO_KIND_PPS || kind == PDO_KIND_EPR_AVS || kind == PDO_KIND_SPR_AVS;
}

static bool on_adjustable(ch_id_t ch)
{
    const pd_node_t *n = pd_node_get(ch);
    return n->contract_valid && kind_adjustable(n->contract_kind);
}

// PDO selection for this channel at mv: PPS preferred; without PPS stay on the fixed PDO (anchor, only the other channel is adjusted); AVS only if neither exists.
// Returns false = no usable PDO. When the capability table is unknown (V203 started later than X035, EPR), leave the decision to X035 (adjustable PDO)
static bool pick_kind(ch_id_t ch, uint16_t mv, bool *anchor, uint16_t *max_ma)
{
    const pd_node_t *n = pd_node_get(ch);
    *anchor = false;
    *max_ma = 0;
    if (n->n_caps == 0 || n->epr) return true;

    const pdo_info_t *pps = NULL, *avs = NULL, *fix = NULL;
    for (uint8_t i = 0; i < n->n_caps; i++)
    {
        const pdo_info_t *c = &n->caps[i];
        if (mv < c->min_mv || mv > c->max_mv) continue;
        if (c->kind == PDO_KIND_PPS && !pps) pps = c;
        else if (kind_adjustable(c->kind) && c->kind != PDO_KIND_PPS && !avs) avs = c;
        else if (c->kind == PDO_KIND_FIXED && !fix) fix = c;
    }
    const pdo_info_t *c = pps ? pps : fix ? fix : avs;
    if (!c) return false;
    *anchor = (c == fix);
    *max_ma = c->max_ma;
    return true;
}

// Allowed offset range: SHARE_RANGE_MV, leaving a 100mV margin so as not to trigger the protection voltage window
static int16_t range_mv(void)
{
    int32_t win = (int32_t)sh.v0 * PROT_VWIN_HIGH_PCT / 100 + PROT_VWIN_MARGIN_MV - 100;
    int32_t r = win < SHARE_RANGE_MV ? win : SHARE_RANGE_MV;
    r -= r % SHARE_STEP_MV;
    return (int16_t)(r > 0 ? r : 0);
}

// Downward range for follow-PDO mode: SHARE_FOLLOW_DOWN_MV, and not below the protection voltage window's lower limit (reserving another 500mV for line loss)
static int16_t range_down_mv(void)
{
    if (sh.mode != SHARE_MODE_FOLLOW) return range_mv();
    int32_t win = (int32_t)sh.v0 * PROT_VWIN_LOW_PCT / 100 + PROT_VWIN_MARGIN_MV - 100 - 500;
    int32_t r = win < SHARE_FOLLOW_DOWN_MV ? win : SHARE_FOLLOW_DOWN_MV;
    if (r < range_mv()) r = range_mv();
    r -= r % SHARE_STEP_MV;
    return (int16_t)(r > 0 ? r : 0);
}

// Round the offset toward 0 by the step
static int16_t step_floor(int32_t v)
{
    return (int16_t)(v - v % SHARE_STEP_MV);
}

// Adjustable PDO used by this channel at mv: the PDO of the current contract is preferred (X035 stays on the current PDO when fine-tuning), otherwise an adjustable PDO whose voltage range contains mv;
// returns NULL if the capability table is unknown
static const pdo_info_t *adj_cap(ch_id_t ch, uint16_t mv)
{
    const pd_node_t *n = pd_node_get(ch);
    const pdo_info_t *c = NULL;
    if (n->n_caps)      // X035 2.1+ reports the EPR capability table in EPR mode (positions consistent with the contract)
    {
        if (n->contract_pos >= 1 && n->contract_pos <= n->n_caps &&
            kind_adjustable(n->caps[n->contract_pos - 1].kind))
        {
            c = &n->caps[n->contract_pos - 1];
        }
        for (uint8_t i = 0; !c && i < n->n_caps; i++)
        {
            if (kind_adjustable(n->caps[i].kind) && mv >= n->caps[i].min_mv && mv <= n->caps[i].max_mv)
                c = &n->caps[i];
        }
    }
    return c;
}

// Offset range per channel: range_mv() intersected with that channel's adjustable PDO voltage range; when the capability table is unknown, only limited by range_mv()
static void calc_limits(ch_id_t ch)
{
    int32_t hi = range_mv(), lo = -range_down_mv();

    if (sh.anchor[ch])
    {
        sh.lim_lo[ch] = sh.lim_hi[ch] = 0;
        return;
    }

    const pdo_info_t *c = adj_cap(ch, sh.v0);
    if (c)
    {
        int32_t cap_hi = (int32_t)c->max_mv - sh.v0, cap_lo = (int32_t)c->min_mv - sh.v0;
        if (cap_hi < hi) hi = cap_hi;
        if (cap_lo > lo) lo = cap_lo;
    }
    sh.lim_lo[ch] = (lo > 0) ? 0 : step_floor(lo);
    sh.lim_hi[ch] = (hi < 0) ? 0 : step_floor(hi);
}

static void stop(const char *reason)
{
    if (sh.state != SHARE_OFF) LOG("SHARE off: %s (adj=%lu)", reason ? reason : "-", sh.adjustments);
    sh.state = SHARE_OFF;
    sh.reason = reason;
    sh.adj_pending = false;
    sh.adj_mask = 0;
}

// Error: if any channel is already on an adjustable PDO, switch it back to the fixed PDO first, then stop.
// count = false is used for external events (unplug, disconnect), not counted toward auto-entry failures
static void fail_ex(const char *reason, bool count)
{
    LOG("SHARE fault: %s", reason);
    sh.reason = reason;
    if (count && sh.auto_on && ++sh.auto_faults >= SHARE_AUTO_MAX_FAULTS)
    {
        sh.auto_gave_up = true;
        LOG("SHARE auto gave up after %u faults", sh.auto_faults);
    }
    if (on_adjustable(CH1) || on_adjustable(CH2))
    {
        sh.state = SHARE_EXIT;
        sh.step_ch = CH1;
        sh.step_sent = false;
    }
    else
    {
        stop(reason);
    }
}

static void fail(const char *reason)
{
    fail_ex(reason, true);
}

// Enter / resume RUN: clear transient, assist, pending-measurement and waiting state (v0.0.102: resuming from HOLD previously missed these,
// so the OCP assist and transient reference from before the dropout were misused at the first evaluation after resuming)
static void run_reset(uint32_t now_ms)
{
    sh.state = SHARE_RUN;
    sh.adj_pending = false;
    sh.adj_mask = 0;
    sh.saturated = false;
    sh.last_adj_ms = now_ms;
    sh.settle_until = now_ms + SHARE_SETTLE_MS;
    sh.est_pending = false;
    sh.urgent_ch = -1;
    sh.prev_total = 0;
    sh.inst_prev = 0;
    sh.fast_ref = 0;
    sh.trim_since = 0;
    sh.idle_since = 0;
}

static void send_step(ch_id_t ch, uint8_t code, uint16_t mv, uint32_t now_ms)
{
    const pd_node_t *n = pd_node_get(ch);
    sh.seq_mark = n->contract_seq;
    sh.t_mark = now_ms;
    sh.step_sent = pd_node_send_pd_cmd(ch, code, mv);
}

// Switch in / switch out: send channel by channel and wait for the new contract (avoids both channels changing at the same time)
static void process_switch(uint32_t now_ms)
{
    bool entering = (sh.state == SHARE_ENTER);
    ch_id_t ch = sh.step_ch;
    const pd_node_t *n = pd_node_get(ch);

    // Target PDO: adjustable PDO when switching in (fixed PDO for anchor), fixed PDO when switching out; channels already on the target PDO are skipped
    bool want_adj = entering && !sh.anchor[ch];
    uint16_t vt = entering ? sh.v0 : sh.v_exit;     // Switching out returns to the contract voltage at entry (the center may have been moved down for headroom)
    int32_t dv0 = (int32_t)n->contract_mv - vt;
    bool done_already = want_adj ? false
                      : entering ? (n->contract_valid && n->contract_kind == PDO_KIND_FIXED &&
                                    dv0 >= -MV_MATCH && dv0 <= MV_MATCH)
                                 : !on_adjustable(ch);
    if (!done_already)
    {
        if (!sh.step_sent)
        {
            send_step(ch, want_adj ? PD_TARGET_ADJ_FIRST : PD_TARGET_FIXED_FIRST, vt, now_ms);
            if (!sh.step_sent)
            {
                if (entering) fail("PD LINK NOT READY");
                else stop(sh.reason ? sh.reason : "PD LINK NOT READY");
                return;
            }
            LOG("SHARE CH%d %s %umV", ch + 1, want_adj ? "-> PPS" : "-> FIXED", vt);
            return;
        }

        uint8_t res = n->last_cmd_result;
        if (res != PD_RESULT_NONE && res != PD_RESULT_OK && res != PD_RESULT_OK_CLAMPED)
        {
            LOG("SHARE CH%d switch rejected: %s", ch + 1, pd_result_str(res));
            if (entering) fail("PPS REQUEST REJECTED");
            else stop(sh.reason ? sh.reason : "EXIT REJECTED");
            return;
        }

        int32_t dv = (int32_t)n->contract_mv - vt;
        bool arrived = n->contract_seq != sh.seq_mark && n->contract_valid &&
                       (want_adj ? kind_adjustable(n->contract_kind) : !kind_adjustable(n->contract_kind)) &&
                       dv >= -MV_MATCH && dv <= MV_MATCH;
        if (!arrived)
        {
            if ((uint32_t)(now_ms - sh.t_mark) > SHARE_SWITCH_TIMEOUT_MS)
            {
                if (entering) fail("PPS ENTER TIMEOUT");
                else stop(sh.reason ? sh.reason : "EXIT TIMEOUT");
            }
            return;
        }
    }

    // This channel done, handle the next one
    sh.step_sent = false;
    if (ch == CH1)
    {
        sh.step_ch = CH2;
        return;
    }
    if (entering)
    {
        run_reset(now_ms);
        sh.off[CH1] = sh.off[CH2] = 0;
        sh.fails[CH1] = sh.fails[CH2] = 0;
        sh.trim = 0;
        sh.trim_dir = 0;
        if (!sh.k) sh.k = SHARE_K_INIT_MA_PER_V;
        sh.run_since = now_ms ? now_ms : 1;
        calc_limits(CH1);
        calc_limits(CH2);
        LOG("SHARE run: center %umV, CH1 %d..%d CH2 %d..%dmV", sh.v0,
            sh.lim_lo[CH1], sh.lim_hi[CH1], sh.lim_lo[CH2], sh.lim_hi[CH2]);
    }
    else
    {
        stop(sh.reason ? sh.reason : "user");
    }
}

// Dropout: link disconnected, contract invalid (unplug / Hard Reset), or path shut off by protection
static bool ch_lost(ch_id_t ch)
{
    return !pd_node_ready(ch) || !pd_node_get(ch)->contract_valid || oring_path_down(ch);
}

static void hold(uint8_t mask, uint32_t now_ms)
{
    static const char *const str[4] = {"", "CH1 LOST, HOLDING", "CH2 LOST, HOLDING", "BOTH LOST, HOLDING"};
    if (sh.state != SHARE_HOLD)
    {
        LOG("SHARE lost 0x%x, hold (v0=%u off=%d/%d)", mask, sh.v0, sh.off[CH1], sh.off[CH2]);
        sh.state = SHARE_HOLD;
        sh.hold_since = now_ms;
        sh.lost_mask = 0;
        sh.step_sent = false;
        sh.back_since = 0;
        sh.adj_pending = false;
        sh.adj_mask = 0;
    }
    sh.lost_mask |= mask;
    sh.reason = str[sh.lost_mask & 3];
}

// HOLD: the channel that did not drop out is left untouched; once a dropped channel gets a contract again and its path is on, send the recorded voltage channel by channel
// (PPS: v0 + offset; anchor: v0 fixed PDO), return to running after all are restored
static void process_hold(uint32_t now_ms)
{
    uint8_t lost = 0;
    for (ch_id_t c = CH1; c < CH_COUNT; c++)
        if (ch_lost(c)) lost |= (uint8_t)(1u << c);
    if (lost & ~sh.lost_mask) hold(lost, now_ms);

    if (!sh.step_sent)
    {
        if ((uint32_t)(now_ms - sh.hold_since) > SHARE_HOLD_TIMEOUT_MS)
        {
            fail_ex("HOLD TIMEOUT", false);
            return;
        }
        // First channel that has come back
        ch_id_t ch = CH_COUNT;
        for (ch_id_t c = CH1; c < CH_COUNT; c++)
            if ((sh.lost_mask & (1u << c)) && !(lost & (1u << c)) && oring_path_is_enabled(c)) { ch = c; break; }
        if (ch == CH_COUNT)
        {
            sh.back_since = 0;
            return;
        }
        if (!sh.back_since) sh.back_since = now_ms ? now_ms : 1;
        if ((uint32_t)(now_ms - sh.back_since) < SHARE_RESUME_DELAY_MS) return;

        // The capability table may have changed after renegotiation (charger swapped): reselect the PDO and limit the offset to the new range
        ch_id_t other = CH_OTHER(ch);
        bool anchor;
        uint16_t max_ma;
        if (!pick_kind(ch, sh.v0, &anchor, &max_ma) || (anchor && sh.anchor[other]))
        {
            fail_ex("NO PPS AFTER RECONNECT", false);
            return;
        }
        sh.anchor[ch] = anchor;
        calc_limits(ch);
        if (anchor) sh.off[ch] = 0;
        else if (sh.off[ch] < sh.lim_lo[ch]) sh.off[ch] = sh.lim_lo[ch];
        else if (sh.off[ch] > sh.lim_hi[ch]) sh.off[ch] = sh.lim_hi[ch];

        uint16_t mv = (uint16_t)(sh.v0 + sh.off[ch]);
        sh.resume_ch = ch;
        send_step(ch, anchor ? PD_TARGET_FIXED_FIRST : PD_TARGET_ADJ_FIRST, mv, now_ms);
        if (!sh.step_sent) return;
        LOG("SHARE CH%d back, resume at %umV%s", ch + 1, mv, anchor ? " (fixed)" : "");
        return;
    }

    ch_id_t ch = sh.resume_ch;
    const pd_node_t *n = pd_node_get(ch);
    // Dropped out again during recovery: go back to waiting
    if (lost & (1u << ch))
    {
        sh.step_sent = false;
        sh.back_since = 0;
        return;
    }
    uint8_t res = n->last_cmd_result;
    if (res != PD_RESULT_NONE && res != PD_RESULT_OK && res != PD_RESULT_OK_CLAMPED)
    {
        fail_ex("RESUME REJECTED", false);
        return;
    }
    int32_t dv = (int32_t)n->contract_mv - (sh.v0 + sh.off[ch]);
    bool arrived = n->contract_seq != sh.seq_mark &&
                   (sh.anchor[ch] ? !kind_adjustable(n->contract_kind) : kind_adjustable(n->contract_kind)) &&
                   dv >= -MV_MATCH && dv <= MV_MATCH;
    if (!arrived)
    {
        if ((uint32_t)(now_ms - sh.t_mark) > SHARE_SWITCH_TIMEOUT_MS) fail_ex("RESUME TIMEOUT", false);
        return;
    }

    sh.lost_mask &= (uint8_t)~(1u << ch);
    sh.step_sent = false;
    sh.back_since = 0;
    sh.fails[ch] = 0;
    LOG("SHARE CH%d resumed after %lums", ch + 1, now_ms - sh.hold_since);
    if (sh.lost_mask) return;

    run_reset(now_ms);
    sh.reason = NULL;
}

// Voltage step of this channel: PPS 20mV, AVS 100mV
static int32_t quant(ch_id_t ch)
{
    pdo_kind_t k = pd_node_get(ch)->contract_kind;
    return (k == PDO_KIND_SPR_AVS || k == PDO_KIND_EPR_AVS) ? SHARE_AVS_STEP_MV : SHARE_STEP_MV;
}

// Offset of this channel after moving by step in direction dir (-1 down / +1 up): align the absolute voltage to the channel's step,
// when out of range take the outermost aligned value within range; return false if it cannot move (already at the boundary)
static bool next_offset(ch_id_t ch, int8_t dir, int16_t step, int16_t *out)
{
    int32_t q = quant(ch);
    int32_t s = (step < q) ? q : step - step % q;
    int32_t cur = sh.v0 + sh.off[ch];
    int32_t edge = sh.v0 + ((dir < 0) ? sh.lim_lo[ch] : sh.lim_hi[ch]);
    int32_t mv = cur + dir * s;

    if (dir < 0)
    {
        if (mv < edge) mv = edge;
        mv = (mv + q - 1) / q * q;          // Align upward, not crossing the lower limit
        if (mv > cur - q) mv = (cur - 1) / q * q;   // Move at least to the next aligned point
        if (mv < edge) return false;
    }
    else
    {
        if (mv > edge) mv = edge;
        mv = mv / q * q;                    // Align downward, not crossing the upper limit
        if (mv < cur + q) mv = (cur / q + 1) * q;
        if (mv > edge) return false;
    }
    *out = (int16_t)(mv - sh.v0);
    return true;
}

// Capability table fingerprint (FNV-1a): binds learning results to the charger; returns 0 if the capability table is unknown
static uint32_t caps_fp(ch_id_t ch)
{
    const pd_node_t *n = pd_node_get(ch);
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < n->n_caps; i++)
    {
        const pdo_info_t *c = &n->caps[i];
        uint32_t v[3] = {(uint32_t)c->kind, ((uint32_t)c->min_mv << 16) | c->max_mv, c->max_ma};
        for (uint8_t j = 0; j < 3; j++) h = (h ^ v[j]) * 16777619u;
    }
    return n->n_caps ? h : 0;
}

static bool profile_match(void)
{
    const share_profile_t *pf = &settings_get()->profile;
    int32_t dv = (int32_t)sh.v0 - pf->v0;
    return pf->valid && pf->fp[CH1] && pf->fp[CH1] == caps_fp(CH1) && pf->fp[CH2] == caps_fp(CH2) &&
           dv >= -SHARE_MANUAL_V0_TOL_MV && dv <= SHARE_MANUAL_V0_TOL_MV;
}

bool share_manual_ff(void)
{
    return sh.state == SHARE_RUN && sh.mode == SHARE_MODE_MANUAL && !share_learn.active && profile_match();
}

// Learning: when the closed loop has converged as far as it can (within deadband, or should not move further after quantization), more than SHARE_LEARN_STABLE_MS has passed since the last voltage adjustment and load transient,
// and both channels are sharing, record the offset difference; the residual error is converted by gain k as a correction (limited to ±100mV)
static void learn_record(uint32_t now_ms, int32_t i1, int32_t i2, int32_t err)
{
    int32_t total = i1 + i2, mn = i1 < i2 ? i1 : i2;
    uint32_t b = (uint32_t)total / SHARE_LEARN_BIN_MA;
    if (!share_learn.active || !sh.k_learned || b >= SHARE_LEARN_BINS ||
        (uint32_t)(now_ms - sh.t_mark) < SHARE_LEARN_STABLE_MS ||
        (uint32_t)(now_ms - sh.transient_ms) < SHARE_LEARN_STABLE_MS ||
        mn * 100 < total * SHARE_K_EST_MIN_PCT)
        return;
    int32_t corr = err * 1000 / sh.k;
    if (corr > 100) corr = 100;
    if (corr < -100) corr = -100;
    share_learn_bin_t *e = &share_learn.bin[b];
    if (e->n == 0xFFFF) return;
    e->sum_ma += total;
    e->sum_mv += sh.off[CH1] - sh.off[CH2] - corr;
    e->n++;
}

// tag: records which path sent it (SHARE_HIST_* bits, for diagnostics)
// Send one channel's offset adjustment (sh.off[ch] already updated to the new value, prev is the value before the adjustment), wait for that channel's new contract; both channels may be in flight at once
static bool send_adj(ch_id_t ch, int16_t prev, uint32_t now_ms)
{
    const pd_node_t *n = pd_node_get(ch);
    sh.adj_prev2[ch] = prev;
    sh.adj_seq[ch] = n->contract_seq;
    sh.t_mark = now_ms;
    if (!pd_node_send_pd_cmd(ch, PD_TARGET_ADJ_FIRST, (uint16_t)(sh.v0 + sh.off[ch]))) return false;
    sh.adj_mask |= (uint8_t)(1u << ch);
    sh.adj_pending = true;
    sh.adj_ch = ch;
    sh.adjustments++;
    return true;
}

static void issue(ch_id_t ch, uint32_t now_ms, int32_t i1, int32_t i2, uint8_t tag)
{
    hist_add(now_ms, i1, i2, (uint8_t)((ch + 1) | tag));
    if (!send_adj(ch, sh.adj_prev, now_ms))
    {
        fail("PD LINK NOT READY");
        return;
    }
    LOG("SHARE I=%ld/%ldmA -> CH%d %umV (off %d/%d)", i1, i2, ch + 1, sh.v0 + sh.off[ch], sh.off[CH1], sh.off[CH2]);
}

// Limit this channel's offset target to its range, then align it toward 0 by the channel's step
static int16_t clamp_off(ch_id_t ch, int32_t target)
{
    int32_t q = quant(ch);
    if (target > sh.lim_hi[ch]) target = sh.lim_hi[ch];
    if (target < sh.lim_lo[ch]) target = sh.lim_lo[ch];
    return (int16_t)(target / q * q);
}

// Send both channels at once (each has already updated sh.off; a channel that did not change is not sent)
static void issue2(uint32_t now_ms, int32_t i1, int32_t i2, int16_t prev1, int16_t prev2, uint8_t tag)
{
    bool m1 = sh.off[CH1] != prev1, m2 = sh.off[CH2] != prev2;
    hist_add(now_ms, i1, i2, (uint8_t)((m1 && m2 ? 5 : m1 ? 1 : 2) | tag));
    if ((m1 && !send_adj(CH1, prev1, now_ms)) || (m2 && !send_adj(CH2, prev2, now_ms)))
    {
        fail("PD LINK NOT READY");
        return;
    }
    LOG("SHARE I=%ld/%ldmA -> %umV/%umV (off %d/%d)", i1, i2, sh.v0 + sh.off[CH1], sh.v0 + sh.off[CH2], sh.off[CH1], sh.off[CH2]);
}

static void saturate(uint32_t now_ms, int32_t i1, int32_t i2)
{
    if (!sh.saturated) LOG("SHARE saturated: I=%ld/%ldmA off=%d/%dmV", i1, i2, sh.off[CH1], sh.off[CH2]);
    sh.saturated = true;
    hist_add(now_ms, i1, i2, 3);
}

// Light-load pre-bias current: sum of both channels' contract currents × SHARE_FF_PRE_PCT%; returns 0 if contract currents are unknown (no pre-bias)
static int32_t ff_pre_ma(void)
{
    int32_t c1 = pd_node_get(CH1)->contract_ma, c2 = pd_node_get(CH2)->contract_ma;
    if (c1 <= 0 || c2 <= 0) return 0;
    return (c1 + c2) * SHARE_FF_PRE_PCT / 100;
}

// Load transient: invalidate the over-limit record, pending gain measurement and trim timer, and start the diagnostic trace
static void mark_transient(uint32_t now_ms)
{
    sh.est_pending = false;
    sh.ol_dir = 0;
    sh.transient_ms = now_ms;
    sh.trim_since = 0;
}

// MANUAL feed-forward target offset difference (CH1 − CH2, mV) = learned line a + k × I_total + intercept trim.
// When total current is below ff_pre_ma(), use the line's value at ff_pre_ma() (pre-bias): on a sudden load up to near full load it is only about one step off.
// The position must be taken in advance -- charger A self-protects after about 80ms at ≥6.4A, and a correction taking contract 30ms + charger ~100ms is too late.
// The favored channel does not exceed SHARE_FF_PRE_MAX_PCT% of its contract current (estimated with SHARE_GAIN_MA_PER_V)
static int32_t ff_want(int32_t total)
{
    const share_profile_t *pf = &settings_get()->profile;
    int32_t want = pf->a_mv + (int32_t)pf->k_mv_per_a * total / 1000 + sh.trim;
    int32_t pre = ff_pre_ma();
    if (total >= pre) return want;
    int32_t extra = (int32_t)pf->k_mv_per_a * (pre - total) / 1000;
    ch_id_t fav = (extra > 0) ? CH1 : CH2;
    int32_t room = (int32_t)pd_node_get(fav)->contract_ma * SHARE_FF_PRE_MAX_PCT / 100 - share_target_ma(fav, total);
    int32_t cap = (room > 0) ? room * 1000 / SHARE_GAIN_MA_PER_V : 0;
    if (extra > cap) extra = cap;
    if (extra < -cap) extra = -cap;
    return want + extra;
}

// Intercept trim (only at the periodic evaluation and when already at the target): in the high-load region (≥ ff_pre_ma(); at light load the distribution is bistable and the error is meaningless) the error stays
// outside the deadband for SHARE_TRIM_MS, and one step is expected to reduce the error (SHARE_GAIN_MA_PER_V × one step ≤ SHARE_PREDICT_PCT% × |err|), then correct one step.
// Under quantization the best PDO step may have a sizable error on both sides: a reverse correction (back to the previous step) is made only if the load has not changed and the error is larger than before the last correction,
// not depending on how accurate the gain estimate is. Before v0.0.101 the closed loop's sh.k was used (MANUAL never estimates it, so it stayed at the initial value 5000, actually about 9000),
// at 8A both −400 / −500mV steps were judged movable, flipping back and forth every 3s.
// The correction applies to the whole line, including the light-load pre-bias (which is exactly the position taken in advance for high load)
static void ff_trim(uint32_t now_ms, int32_t i1, int32_t i2, int32_t q)
{
    int32_t total = i1 + i2;
    int32_t err = i1 - share_target_ma(CH1, total);
    int32_t err_abs = err < 0 ? -err : err;
    bool off = total >= SHARE_MIN_TOTAL_MA && total >= ff_pre_ma() && err_abs * 2 * 100 > total * SHARE_DIFF_PCT &&
               SHARE_GAIN_MA_PER_V * q / 1000 * 100 <= err_abs * SHARE_PREDICT_PCT;
    if (!off)
    {
        sh.trim_since = 0;
        return;
    }
    if (!sh.trim_since) sh.trim_since = now_ms ? now_ms : 1;
    if ((uint32_t)(now_ms - sh.trim_since) < SHARE_TRIM_MS) return;
    sh.trim_since = 0;
    int8_t dir = (err > 0) ? -1 : 1;                // CH1 carrying more -> offset difference decreases
    int32_t dt = total - sh.trim_total0;
    if (dt < 0) dt = -dt;
    if (dir == -sh.trim_dir && dt * 100 <= sh.trim_total0 * SHARE_FF_STABLE_PCT && err_abs <= sh.trim_err0) return;
    int32_t t = sh.trim + dir * q;
    if (t > SHARE_TRIM_MAX_MV) t = SHARE_TRIM_MAX_MV;
    if (t < -SHARE_TRIM_MAX_MV) t = -SHARE_TRIM_MAX_MV;
    if (t == sh.trim) return;
    sh.trim = (int16_t)t;
    sh.trim_dir = dir;
    sh.trim_err0 = err_abs;
    sh.trim_total0 = total;
    LOG("SHARE manual trim %dmV (I=%ld/%ldmA)", sh.trim, i1, i2);
}

// MANUAL feed-forward step: when the offset difference is ≥ 0.7 step from the target, both channels move in opposite directions by half each (v0.0.97, twice the correction per contract round);
// when already at the target, the periodic evaluation (tag = 0) then checks the intercept trim. tag is recorded in history (SHARE_HIST_URGENT / _FAST).
// uch ≥ 0 (OCP assist): the target is reached (or in the opposite direction) but that channel is actually over the limit -- right after a jump the distribution is worse than the static line
// (measured 1→7A: pre-bias already in place, still 5.7/1.3A) -- still move one step in the direction that relieves that channel
static void ff_step(uint32_t now_ms, int32_t i1, int32_t i2, int8_t uch, uint8_t tag)
{
    int32_t d = ff_want(i1 + i2) - (sh.off[CH1] - sh.off[CH2]);
    int32_t d_abs = d < 0 ? -d : d;
    int32_t q = quant(CH1) > quant(CH2) ? quant(CH1) : quant(CH2);

    // Offset difference increases = current moves toward CH1
    if (uch >= 0 && (d_abs * 10 < q * 7 || (d > 0) != (uch == CH2)))
    {
        d = (uch == CH2) ? q : -q;
        d_abs = q;
    }
    if (d_abs * 10 < q * 7)
    {
        hist_add(now_ms, i1, i2, 0);
        if (!tag) ff_trim(now_ms, i1, i2, q);
        return;
    }
    // When the step count is odd, the extra step goes to the channel that brings the sum of both offsets closer to 0; if one channel hits its limit, the remainder goes to the other
    ch_id_t up = (d > 0) ? CH1 : CH2, dn = CH_OTHER(up);
    int32_t steps = (d_abs + q / 2) / q;
    int32_t n_dn = steps / 2, n_up = steps - n_dn;          // By default the extra step goes to up (when the sum is negative)
    if ((steps & 1) && sh.off[CH1] + sh.off[CH2] > 0) { n_dn = n_up; n_up = steps / 2; }
    int16_t prev1 = sh.off[CH1], prev2 = sh.off[CH2];
    int16_t o_up = clamp_off(up, sh.off[up] + n_up * q);
    int16_t o_dn = clamp_off(dn, sh.off[dn] - n_dn * q);
    int32_t got = (o_up - sh.off[up]) + (sh.off[dn] - o_dn);
    if (got < steps * q)
    {
        int32_t rem = steps * q - got;
        int16_t t = clamp_off(up, o_up + rem);
        rem -= t - o_up;
        o_up = t;
        if (rem > 0) o_dn = clamp_off(dn, o_dn - rem);
    }
    if (o_up == sh.off[up] && o_dn == sh.off[dn])
    {
        saturate(now_ms, i1, i2);
        return;
    }
    sh.off[up] = o_up;
    sh.off[dn] = o_dn;
    sh.saturated = false;
    sh.trim_since = 0;
    issue2(now_ms, i1, i2, prev1, prev2, tag);
}

// Triggers of MANUAL feed-forward (cleaned up in v0.0.102, all sent only via ff_step):
//   - OCP assist: a channel's instantaneous current exceeds the OCP threshold (notified by oring) and ≥ SHARE_URGENT_GAP_MS since the last send → based on instantaneous current
//   - Fast transient: instantaneous total current differs from the average by > SHARE_TRANSIENT_PCT% (and > SHARE_FF_STABLE_MIN_MA), two consecutive samples differ by
//     ≤ SHARE_FF_STABLE_PCT% (settled) → based on instantaneous current, without waiting for the period or the average (v0.0.99); after positioning once, until the average catches up
//     it must change by another SHARE_TRANSIENT_PCT% to count as a new transient (v0.0.101: after 8→1A it positioned once every 10ms while the average lagged)
//   - Periodic: after the settle wait, every SHARE_PERIOD_MS based on the average current; if still changing (partway up a load ramp), check again after SHARE_FF_RECHECK_MS.
//     Only the periodic evaluation performs the intercept trim
static void run_ff(uint32_t now_ms, bool urgent)
{
    const meter_ch_t *m1 = meter_get(CH1);
    const meter_ch_t *m2 = meter_get(CH2);
    int32_t ti = m1->current_ma + m2->current_ma, ta = m1->current_ma_avg + m2->current_ma_avg;
    int32_t dta = ti - ta, dti = ti - sh.inst_prev, dtr = ti - sh.fast_ref;
    sh.inst_prev = ti;
    if (dta < 0) dta = -dta;
    if (dti < 0) dti = -dti;
    if (dtr < 0) dtr = -dtr;
    if (dta * 100 <= ta * SHARE_TRANSIENT_PCT) sh.fast_ref = 0;
    bool fast = !urgent && dta > SHARE_FF_STABLE_MIN_MA && dta * 100 > ta * SHARE_TRANSIENT_PCT &&
                dti * 100 <= ti * SHARE_FF_STABLE_PCT && (uint32_t)(now_ms - sh.t_mark) >= SHARE_URGENT_GAP_MS &&
                (sh.fast_ref == 0 || dtr * 100 > sh.fast_ref * SHARE_TRANSIENT_PCT);
    if (urgent || fast)
    {
        int8_t uch = urgent ? sh.urgent_ch : -1;
        uint8_t tag = urgent ? SHARE_HIST_URGENT : SHARE_HIST_FAST;
        sh.urgent_ch = -1;
        sh.last_adj_ms = now_ms;
        mark_transient(now_ms);
        sh.prev_total = ti;                 // Once the average has caught up, it is no longer treated as a new transient
        sh.fast_ref = ti ? ti : 1;
        hist_add(now_ms, m1->current_ma, m2->current_ma, (uint8_t)(4 | tag));
        ff_step(now_ms, m1->current_ma, m2->current_ma, uch, tag);
        return;
    }
    if ((int32_t)(now_ms - sh.settle_until) < 0) return;
    if ((uint32_t)(now_ms - sh.last_adj_ms) < SHARE_PERIOD_MS) return;
    sh.last_adj_ms = now_ms;

    int32_t i1 = m1->current_ma_avg, i2 = m2->current_ma_avg, total = i1 + i2;
    int32_t prev = sh.prev_total, dtot = total - prev;
    sh.prev_total = total;
    if (dtot < 0) dtot = -dtot;
    if (prev > 0 && dtot * 100 > prev * SHARE_TRANSIENT_PCT)
    {
        mark_transient(now_ms);
        hist_add(now_ms, i1, i2, 4);
    }
    if (prev > 0 && dtot > SHARE_FF_STABLE_MIN_MA && dtot * 100 > prev * SHARE_FF_STABLE_PCT)
    {
        sh.last_adj_ms = now_ms - SHARE_PERIOD_MS + SHARE_FF_RECHECK_MS;
        return;
    }
    ff_step(now_ms, i1, i2, -1, 0);
}

static void process_run(uint32_t now_ms)
{
    // Dropout (link disconnected, contract invalidated by unplug / Hard Reset, path shut off by protection): enter HOLD keeping the records, stop adjusting;
    // if the contract is still valid but the PDO changed (renegotiated), exit through the normal flow
    uint8_t lost = 0;
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
        if (ch_lost(ch)) lost |= (uint8_t)(1u << ch);
    if (lost)
    {
        hold(lost, now_ms);
        return;
    }
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        if (!sh.anchor[ch] && !on_adjustable(ch)) { fail_ex("PPS CONTRACT LOST", false); return; }
        if (sh.anchor[ch] && on_adjustable(ch)) { fail_ex("FIXED CONTRACT LOST", false); return; }
    }

#if SHARE_HIST_ENABLE
    // Diagnostic trace: within SHARE_TRACE_MS after a load transient, record both channels' instantaneous current every SHARE_TRACE_STEP_MS
    if ((uint32_t)(now_ms - sh.transient_ms) < SHARE_TRACE_MS && (uint32_t)(now_ms - sh.trace_ms) >= SHARE_TRACE_STEP_MS)
    {
        sh.trace_ms = now_ms;
        hist_add(now_ms, meter_get(CH1)->current_ma, meter_get(CH2)->current_ma, 6);
    }
#endif
    // Adjustments already sent (both channels may be in flight at once): wait for the new contract per channel; on rejection / timeout restore that channel's offset
    for (ch_id_t ch = CH1; sh.adj_mask && ch < CH_COUNT; ch++)
    {
        if (!(sh.adj_mask & (1u << ch))) continue;
        const pd_node_t *n = pd_node_get(ch);
        uint8_t res = n->last_cmd_result;
        if (res != PD_RESULT_NONE && res != PD_RESULT_OK && res != PD_RESULT_OK_CLAMPED)
        {
            LOG("SHARE CH%d adjust rejected: %s", ch + 1, pd_result_str(res));
            // Restore the offset, and narrow the rejected direction to the value before the adjustment (e.g. the source's PPS upper limit happens to be the center voltage)
            if (sh.off[ch] > sh.adj_prev2[ch]) sh.lim_hi[ch] = sh.adj_prev2[ch];
            else                               sh.lim_lo[ch] = sh.adj_prev2[ch];
            sh.off[ch] = sh.adj_prev2[ch];
            sh.adj_mask &= (uint8_t)~(1u << ch);
            sh.est_pending = false;
            if (++sh.fails[ch] >= SHARE_MAX_FAILS) { fail("ADJUST REJECTED"); return; }
            continue;
        }
        if (n->contract_seq == sh.adj_seq[ch])
        {
            if ((uint32_t)(now_ms - sh.t_mark) > SHARE_ACK_TIMEOUT_MS)
            {
                sh.off[ch] = sh.adj_prev2[ch];
                sh.adj_mask &= (uint8_t)~(1u << ch);
                sh.est_pending = false;
                if (++sh.fails[ch] >= SHARE_MAX_FAILS) { fail("ADJUST TIMEOUT"); return; }
            }
            continue;
        }
        sh.adj_mask &= (uint8_t)~(1u << ch);
        sh.fails[ch] = 0;
        // When AVS changes voltage the charger ramps slowly, so current redistributes more slowly than with PPS: wait longer before measuring, otherwise gain k would be underestimated
        pdo_kind_t kd = n->contract_kind;
        sh.settle_until = now_ms + ((kd == PDO_KIND_SPR_AVS || kd == PDO_KIND_EPR_AVS) ? SHARE_SETTLE_AVS_MS : SHARE_SETTLE_MS);
    }
    sh.adj_pending = sh.adj_mask != 0;
    if (sh.adj_mask) return;
    // User manually turned off a channel's output (protection shutoff already entered HOLD above): pause adjustment, freeze offsets, continue after it is turned on again
    if (!oring_path_is_enabled(CH1) || !oring_path_is_enabled(CH2))
    {
        sh.est_pending = false;
        sh.prev_total = 0;
        // After turning on again, first wait for the current average to catch up (about 80ms low-pass), otherwise the first evaluation would see the turned-off channel still at 0
        sh.settle_until = now_ms + SHARE_SETTLE_MS;
        return;
    }
    // OCP assist (oring notifies when a channel's instantaneous current exceeds the OCP threshold): at least SHARE_URGENT_GAP_MS since the last send
    bool urgent = sh.urgent_ch >= 0 && (uint32_t)(now_ms - sh.t_mark) >= SHARE_URGENT_GAP_MS;
    // If that channel is no longer over the limit during the wait interval: discard, otherwise it would take one extra step (v0.0.100; measured 0→6A cold step +200ms took one extra step)
    if (urgent && meter_get((ch_id_t)sh.urgent_ch)->current_ma <= oring_ocp_limit_ma((ch_id_t)sh.urgent_ch))
    {
        sh.urgent_ch = -1;
        urgent = false;
    }
    const meter_ch_t *m1 = meter_get(CH1);
    const meter_ch_t *m2 = meter_get(CH2);
    if (!m1->online || !m2->online) return;

    // Offsets are based on the contract's actual voltage (the contract report gives the RDO voltage), avoiding getting out of sync with the source state
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        const pd_node_t *n = pd_node_get(ch);
        int32_t off = (int32_t)n->contract_mv - sh.v0;
        if (off >= -SHARE_FOLLOW_DOWN_MV - MV_MATCH && off <= SHARE_RANGE_MV + MV_MATCH) sh.off[ch] = (int16_t)off;
    }

    if (share_manual_ff())
    {
        run_ff(now_ms, urgent);
        return;
    }

    // Below is the closed loop (normal / follow; MANUAL while learning or when the learning result does not apply): OCP assist can skip the settle wait and the period
    if (!urgent && (int32_t)(now_ms - sh.settle_until) < 0) return;
    if (!urgent && (uint32_t)(now_ms - sh.last_adj_ms) < SHARE_PERIOD_MS) return;
    int8_t uch = urgent ? sh.urgent_ch : -1;
    sh.urgent_ch = -1;
    sh.last_adj_ms = now_ms;

    int32_t i1 = m1->current_ma_avg, i2 = m2->current_ma_avg;
    int32_t total = i1 + i2;
    // err = current CH1 carries above its target (in normal mode = (i1 - i2) / 2), 2 × err is compared against total current
    int32_t err = i1 - share_target_ma(CH1, total);
    int32_t err_abs = err < 0 ? -err : err;
    // Load transient (sudden load / unload, a channel's charger losing output): do not adjust this time, and do not use it to estimate gain
    int32_t prev = sh.prev_total, dtot = total - prev;
    sh.prev_total = total;
    if (prev > 0 && (dtot < 0 ? -dtot : dtot) * 100 > prev * SHARE_TRANSIENT_PCT)
    {
        mark_transient(now_ms);
        hist_add(now_ms, i1, i2, (uint8_t)(4 | (urgent ? SHARE_HIST_URGENT : 0)));
        return;
    }
    // OCP assist uses instantaneous current instead (v0.0.97, INA once every 10ms; the over-limit itself is judged on instantaneous values):
    // the average (low-pass about 80ms) lags after a load jump, and waiting for it to catch up before positioning is at least 0.2s late
    if (urgent)
    {
        i1 = m1->current_ma;
        i2 = m2->current_ma;
        total = i1 + i2;
        err = i1 - share_target_ma(CH1, total);
        err_abs = err < 0 ? -err : err;
    }
    if (total < SHARE_MIN_TOTAL_MA)
    {
        // After no load for SHARE_IDLE_ZERO_MS, zero the offsets one channel at a time: the offsets were learned from the previous load and are often opposite to the next one,
        // keeping them would bias the next sudden load toward one channel from the start (measured: after 6A CH1 stayed at -400mV, then at 7A CH2 carried 4.8A)
        if (!sh.idle_since) sh.idle_since = now_ms ? now_ms : 1;
        if ((uint32_t)(now_ms - sh.idle_since) < SHARE_IDLE_ZERO_MS) return;
        for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
        {
            if (sh.anchor[ch] || sh.off[ch] == 0) continue;
            sh.adj_prev = sh.off[ch];
            sh.off[ch] = 0;
            sh.est_pending = false;
            hist_add(now_ms, i1, i2, (uint8_t)(ch + 1));
            if (!send_adj(ch, sh.adj_prev, now_ms))
            {
                fail("PD LINK NOT READY");
                return;
            }
            LOG("SHARE idle: CH%d offset %d -> 0", ch + 1, sh.adj_prev);
            return;                         // One channel at a time; handle the other after the new contract is confirmed
        }
        return;
    }
    sh.idle_since = 0;

    // Effect of the previous step: error reduction / voltage change → gain; not used if total current changed by more than 20% (load changing)
    if (sh.est_pending)
    {
        sh.est_pending = false;
        int32_t dt = total - sh.est_total0;
        int32_t red = (sh.est_err0 > 0) ? sh.est_err0 - err : err - sh.est_err0;
        // Both channels must carry ≥ SHARE_K_EST_MIN_PCT% before and after the adjustment: when one channel has almost no current the ideal diode is still cut off (nonlinear region), and the measured gain is several times too small
        int32_t mn = i1 < i2 ? i1 : i2;
        if ((dt < 0 ? -dt : dt) * 5 < sh.est_total0 && red > 0 && sh.est_dv > 0 &&
            sh.est_min0 * 100 >= sh.est_total0 * SHARE_K_EST_MIN_PCT && mn * 100 >= total * SHARE_K_EST_MIN_PCT)
        {
            int32_t km = red * 1000 / sh.est_dv;
            // The first measurement directly replaces the initial value (the initial value is often several times off, and averaging would misjudge the first few steps), then correct by half each time
            sh.k = sh.k_learned ? (sh.k + km) / 2 : km;
            sh.k_learned = true;
            if (sh.k < SHARE_K_MIN_MA_PER_V) sh.k = SHARE_K_MIN_MA_PER_V;
            if (sh.k > SHARE_K_MAX_MA_PER_V) sh.k = SHARE_K_MAX_MA_PER_V;
        }
    }

    uint8_t dead_pct = (sh.mode == SHARE_MODE_FOLLOW) ? SHARE_FOLLOW_DIFF_PCT : SHARE_DIFF_PCT;
    // The channel with current above its target is near / over its contract current: regardless of the deadband, keep transferring current to the other channel
    const pd_node_t *nh = pd_node_get((err > 0) ? CH1 : CH2);
    int32_t ih = (err > 0) ? i1 : i2;
    bool over_limit = nh->contract_ma && ih * 100 > (int32_t)nh->contract_ma * SHARE_LIMIT_PCT;
    // OCP assist: bring that channel's current down to 90% of the OCP threshold in one go
    int32_t need_ma = 0;
    if (urgent)
    {
        // Right after a load jump the average current (about 80ms low-pass) has not caught up yet: the channel over OCP may not yet be the higher one in the average,
        // transferring by the average would go in the opposite direction (measured 1→7A: the average still showed CH1 higher, CH2 was raised one step and shot to 5.76A); wait for the average to catch up
        if (uch != ((err > 0) ? CH1 : CH2)) return;
        need_ma = ih - oring_ocp_limit_ma((err > 0) ? CH1 : CH2) * 9 / 10;
        if (need_ma > 0) over_limit = true;
    }
    if (!over_limit && err_abs * 2 * 100 <= total * dead_pct)
    {
        sh.saturated = false;
        hist_add(now_ms, i1, i2, 0);
        learn_record(now_ms, i1, i2, err);
        return;
    }

    // Step size: the needed voltage computed from the estimated gain × gain percentage (next_offset then rounds it to the channel's quantization step, at least one step)
    int32_t s = err_abs * 1000 / sh.k * SHARE_GAIN_PCT / 100;
    int32_t smax = SHARE_STEP_MAX_MV;
    if (need_ma > 0)
    {
        int32_t su = need_ma * 1000 / sh.k;
        if (su > SHARE_URGENT_STEP_MAX_MV) su = SHARE_URGENT_STEP_MAX_MV;
        if (su > s) s = su;
        smax = SHARE_URGENT_STEP_MAX_MV;
    }
    int16_t step = (int16_t)(s > smax ? smax : s);

    ch_id_t hi = (err > 0) ? CH1 : CH2;
    ch_id_t lo = CH_OTHER(hi);
    int16_t off_lower, off_raise;
    bool can_lower = next_offset(hi, -1, step, &off_lower);
    bool can_raise = next_offset(lo, +1, step, &off_raise);

    // When both actions are possible, choose the one that brings the sum of both offsets closer to 0, avoiding overall voltage drift
    // (follow-PDO only relaxes the downward range: when raising is limited, lower the other channel instead, the overall voltage may drop)
    bool prefer_lower = sh.off[hi] + sh.off[lo] > 0;
    ch_id_t ch;
    if (can_lower && (!can_raise || prefer_lower))
    {
        ch = hi;
        sh.adj_prev = sh.off[hi];
        sh.off[hi] = off_lower;
    }
    else if (can_raise)
    {
        ch = lo;
        sh.adj_prev = sh.off[lo];
        sh.off[lo] = off_raise;
    }
    else
    {
        ch = CH_COUNT;
    }

    // Prediction: this step's voltage change would change the error by k × dv; move only if |err| is expected to shrink below 60% of its value
    // (k × dv ≤ SHARE_PREDICT_PCT% × |err|), otherwise stop. Eliminates back-and-forth oscillation under the quantization step (AVS 100mV):
    // when the error is about half of one step's effect, the old rule (k × dv ≤ 2|err|) judged every step movable and would jump back and forth between two PDO steps (except near the contract limit)
    if (ch != CH_COUNT)
    {
        int32_t dv = sh.off[ch] - sh.adj_prev;
        if (dv < 0) dv = -dv;
        bool hold = !over_limit && sh.k * dv / 1000 * 100 > err_abs * SHARE_PREDICT_PCT;
        // Near full load (both channels close to their contract currents, every step has one channel over 97%): stop at the better step by "maximum excess".
        // Does not depend on gain k (k is often underestimated at full load): if this step is the reverse of the last over-limit adjustment, and the excess of the step left last time
        // is not smaller than now, going back would only be worse -> stop. Measured at 9A it once jumped back and forth every 1.5s between 4.87/4.19A and 3.80/5.27A
        if (over_limit && need_ma <= 0)
        {
            int32_t il = (hi == CH1) ? i2 : i1;
            int32_t xh = ih - (int32_t)nh->contract_ma * SHARE_LIMIT_PCT / 100;
            int32_t xl = il - (int32_t)pd_node_get(lo)->contract_ma * SHARE_LIMIT_PCT / 100;
            int32_t excess = xh > xl ? xh : xl;
            int8_t dir = (hi == CH1) ? 1 : -1;
            if (sh.ol_dir == -dir && (uint32_t)(now_ms - sh.ol_ms) < SHARE_OL_MEMORY_MS && sh.ol_from_excess >= excess)
            {
                hold = true;
                sh.ol_ms = now_ms;          // Keep stopping as long as the condition holds, do not retry because the memory timed out
            }
            else
            {
                sh.ol_dir = dir;
                sh.ol_from_excess = excess;
                sh.ol_ms = now_ms;
            }
        }
        if (hold)
        {
            sh.off[ch] = sh.adj_prev;
            sh.saturated = false;
            hist_add(now_ms, i1, i2, 0);
            if (!over_limit) learn_record(now_ms, i1, i2, err);    // Cannot get any closer after quantization
            return;
        }
        sh.est_pending = true;
        sh.est_err0 = err;
        sh.est_total0 = total;
        sh.est_min0 = i1 < i2 ? i1 : i2;
        sh.est_dv = (int16_t)dv;
    }
    else
    {
        saturate(now_ms, i1, i2);
        return;
    }
    issue(ch, now_ms, i1, i2, (uint8_t)(SHARE_HIST_LOOP | (uch >= 0 ? SHARE_HIST_URGENT : 0)));
}

static void process_auto(uint32_t now_ms);

void share_init(void)
{
    sh = (share_ctx_t){.state = SHARE_OFF, .urgent_ch = -1};
    sh.auto_on = settings_get()->balance_on;    // Must be called after app_init (which reads the settings)
    sh.mode = (settings_get()->balance_mode < SHARE_MODE_COUNT) ? (share_mode_t)settings_get()->balance_mode
                                                                : SHARE_MODE_NORMAL;
#if SHARE_HIST_ENABLE
    share_hist.magic = 0x48495354;
#endif
    const share_profile_t *pf = &settings_get()->profile;
    share_learn.valid = pf->valid;
    share_learn.bins = pf->bins;
    share_learn.a_mv = pf->a_mv;
    share_learn.k_mv_per_a = pf->k_mv_per_a;
    share_learn.resid_mv = pf->resid_mv;
    share_learn.v0 = pf->v0;
}

void share_process(uint32_t now_ms)
{
    process_auto(now_ms);

    switch (sh.state)
    {
        case SHARE_ENTER:
        case SHARE_EXIT:
            process_switch(now_ms);
            break;
        case SHARE_RUN:
            process_run(now_ms);
            break;
        case SHARE_HOLD:
            process_hold(now_ms);
            break;
        default:
            break;
    }
}

// Enable preconditions; returns NULL when met and gives the center voltage and whether each channel stays on the fixed PDO
static const char *check_enter(uint16_t *v0_out, uint16_t *v_exit_out, bool anchor_out[CH_COUNT])
{
    const pd_node_t *n1 = pd_node_get(CH1);
    const pd_node_t *n2 = pd_node_get(CH2);

    if (!pd_node_ready(CH1) || !pd_node_ready(CH2))       return "PD LINK OFFLINE";
    if (!n1->ext_proto || !n2->ext_proto)                 return "X035 FW TOO OLD";
    if (!n1->rdo_volatile || !n2->rdo_volatile)           return "X035 NV NOT OFF";
    if (!n1->contract_valid || !n2->contract_valid)       return "NO CONTRACT";
    if (n1->vset == PD_VSET_BUSY || n2->vset == PD_VSET_BUSY || app_vset_busy()) return "VOLTAGE SWITCHING";
    if (!meter_get(CH1)->online || !meter_get(CH2)->online)   return "NO I-SENSE";
    if (!oring_path_is_enabled(CH1) || !oring_path_is_enabled(CH2)) return "PATH OFF";

    // Voltage difference between the two channels: when one channel is on an adjustable PDO (e.g. V203 restarted alone while X035 still holds the offset from the last current sharing) allow up to 2 × SHARE_RANGE_MV,
    // both channels are set to the center voltage on entry; when both are fixed PDOs they must match
    bool adj1 = kind_adjustable(n1->contract_kind), adj2 = kind_adjustable(n2->contract_kind);
    int32_t dv = (int32_t)n1->contract_mv - n2->contract_mv;
    int32_t tol = (adj1 || adj2) ? 2 * SHARE_RANGE_MV : MV_MATCH;
    if (dv < -tol || dv > tol) return "CH1/CH2 VOLTAGE DIFFER";

    // Center voltage: when one channel is on a fixed contract use it (anchor does not move), otherwise take the average of both contracts, aligned to 20mV
    uint32_t avg = (adj1 && adj2) ? ((uint32_t)n1->contract_mv + n2->contract_mv) / 2 : n1->contract_mv;
    // When one channel is on EPR or AVS, align to 100mV (AVS effective step), otherwise to PPS's 20mV.
    // Before v0.0.88 it was always 20mV: 48.0V / 47.9V averaged to 47.96V, AVS could only give 48.0V, the offset was stuck at +40mV,
    // and idle zeroing resent the request every 1.5s, never finishing (measured 2026-10-25)
    bool avs = n1->epr || n2->epr ||
               n1->contract_kind == PDO_KIND_EPR_AVS || n1->contract_kind == PDO_KIND_SPR_AVS ||
               n2->contract_kind == PDO_KIND_EPR_AVS || n2->contract_kind == PDO_KIND_SPR_AVS;
    uint32_t q = avs ? SHARE_AVS_STEP_MV : SHARE_STEP_MV;
    uint16_t v0 = (uint16_t)((avg + q / 2) / q * q);
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        const pd_node_t *n = pd_node_get(ch);
        if (n->contract_kind == PDO_KIND_FIXED) { v0 = n->contract_mv; break; }
    }
    uint16_t v_exit = v0;
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        uint16_t max_ma;
        if (!pick_kind(ch, v0, &anchor_out[ch], &max_ma)) return "NO PPS AT THIS VOLTAGE";
        if (max_ma && meter_get(ch)->current_ma_avg > max_ma) return "LOAD > PPS CURRENT";
    }
    if (anchor_out[CH1] && anchor_out[CH2]) return "NO PPS AT THIS VOLTAGE";
    // On exit return to v_exit; the center keeps upward headroom (SHARE_CENTER_HEADROOM_MV): when both channels will be on adjustable PDOs (by the PDO selection result and capability table,
    // not the current contract -- after a normal exit both channels sit on fixed PDOs, and v0.0.97 judging by that never moved down) and the center sits at the adjustable PDO's upper limit, move it down;
    // after moving down, reselect PDOs; if that fails, keep the original center
    if (!anchor_out[CH1] && !anchor_out[CH2])
    {
        uint32_t ceil = 0xFFFF;
        for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
        {
            const pdo_info_t *c = adj_cap(ch, v0);
            if (c && c->max_mv < ceil) ceil = c->max_mv;
        }
        if (ceil != 0xFFFF && (uint32_t)v0 + SHARE_CENTER_HEADROOM_MV > ceil)
        {
            uint16_t v1 = (uint16_t)((ceil - SHARE_CENTER_HEADROOM_MV) / q * q);
            bool a[CH_COUNT], ok = true;
            for (ch_id_t ch = CH1; ok && ch < CH_COUNT; ch++)
            {
                uint16_t max_ma;
                ok = pick_kind(ch, v1, &a[ch], &max_ma) && !a[ch];
            }
            if (ok) v0 = v1;
        }
    }
    *v0_out = v0;
    *v_exit_out = v_exit;
    return NULL;
}

static void enter(uint16_t v0, uint16_t v_exit, const bool anchor[CH_COUNT])
{
    sh.state = SHARE_ENTER;
    sh.v0 = v0;
    sh.v_exit = v_exit;
    sh.anchor[CH1] = anchor[CH1];
    sh.anchor[CH2] = anchor[CH2];
    sh.off[CH1] = sh.off[CH2] = 0;
    sh.step_ch = CH1;
    sh.step_sent = false;
    sh.reason = NULL;
    sh.wait_reason = NULL;
    sh.adjustments = 0;
    LOG("SHARE enter at %umV%s", v0, sh.auto_on ? " (auto)" : "");
}

// Auto entry: when the user has enabled it and it is not currently running, enter after the preconditions are continuously met for SHARE_AUTO_SETTLE_MS;
// after a failure back off SHARE_AUTO_RETRY_MS, give up after SHARE_AUTO_MAX_FAULTS consecutive failures
static void process_auto(uint32_t now_ms)
{
    if (!sh.auto_on || sh.auto_gave_up) return;

    // Clear the failure count after running stably for a while
    if (sh.state == SHARE_RUN && sh.auto_faults && (uint32_t)(now_ms - sh.run_since) > SHARE_AUTO_RESET_MS)
        sh.auto_faults = 0;
    if (sh.state != SHARE_OFF) return;

    uint16_t v0, vx;
    bool anchor[CH_COUNT];
    const char *why = check_enter(&v0, &vx, anchor);
    if (why)
    {
        if (why != sh.wait_reason) LOG("SHARE auto waiting: %s", why);
        sh.wait_reason = why;
        sh.auto_ok_since = 0;
        return;
    }
    sh.wait_reason = NULL;
    if (!sh.auto_ok_since) sh.auto_ok_since = now_ms ? now_ms : 1;
    if ((uint32_t)(now_ms - sh.auto_ok_since) < SHARE_AUTO_SETTLE_MS) return;
    if ((int32_t)(now_ms - sh.auto_retry_at) < 0) return;

    sh.auto_ok_since = 0;
    sh.auto_retry_at = now_ms + SHARE_AUTO_RETRY_MS;
    enter(v0, vx, anchor);
}

bool share_request(bool on)
{
    // User action: save the intent and clear the auto give-up state
    sh.auto_on = on;
    sh.auto_gave_up = false;
    sh.auto_faults = 0;
    sh.auto_retry_at = 0;
    settings_set_balance(on);

    if (!on)
    {
        sh.wait_reason = NULL;
        if (sh.state == SHARE_OFF || sh.state == SHARE_EXIT) return true;
        sh.reason = NULL;
        sh.state = SHARE_EXIT;
        sh.step_ch = CH1;
        sh.step_sent = false;
        sh.adj_pending = false;
        sh.adj_mask = 0;
        LOG("SHARE exit requested");
        return true;
    }

    if (sh.state != SHARE_OFF) return true;

    uint16_t v0, vx;
    bool anchor[CH_COUNT];
    const char *deny = check_enter(&v0, &vx, anchor);
    if (deny)
    {
        // Intent kept: auto entry once the conditions are met
        sh.reason = deny;
        sh.wait_reason = deny;
        LOG("SHARE on, waiting: %s", deny);
        return false;
    }
    enter(v0, vx, anchor);
    return true;
}

void share_ocp_assist(ch_id_t ch)
{
    if (sh.state == SHARE_RUN && ch < CH_COUNT) sh.urgent_ch = (int8_t)ch;
}

void share_abort(const char *reason)
{
    if (sh.state == SHARE_OFF) return;
    stop(reason);
}

share_state_t share_state(void)
{
    return sh.state;
}

const char *share_state_str(share_state_t state)
{
    switch (state)
    {
        case SHARE_ENTER: return "ENTER";
        case SHARE_RUN:   return "RUN";
        case SHARE_EXIT:  return "EXIT";
        case SHARE_HOLD:  return "HOLD";
        default:          return "OFF";
    }
}

const char *share_reason(void)
{
    return sh.reason;
}

bool share_auto_on(void)
{
    return sh.auto_on;
}

bool share_auto_gave_up(void)
{
    return sh.auto_gave_up;
}

const char *share_wait_reason(void)
{
    return sh.auto_on ? sh.wait_reason : NULL;
}

share_mode_t share_mode(void)
{
    return sh.mode;
}

void share_set_mode(share_mode_t mode)
{
    if (mode >= SHARE_MODE_COUNT || mode == sh.mode) return;
    sh.mode = mode;
    settings_set_balance_mode((uint8_t)mode);
    if (sh.state == SHARE_RUN)
    {
        calc_limits(CH1);
        calc_limits(CH2);
        sh.fails[CH1] = sh.fails[CH2] = 0;
        sh.saturated = false;
        sh.trim = 0;
        sh.trim_since = 0;
        sh.trim_dir = 0;
        sh.fast_ref = 0;
        sh.prev_total = 0;
    }
    LOG("SHARE mode %s", share_mode_str(mode));
}

const char *share_mode_str(share_mode_t mode)
{
    switch (mode)
    {
        case SHARE_MODE_MANUAL: return "MANUAL";
        case SHARE_MODE_FOLLOW: return "FOLLOW";
        default:                return "NORMAL";
    }
}

// Follow-PDO / MANUAL: in proportion to the two channels' contract currents (for PPS/AVS requests, that PDO's limit); split evenly if either is unknown
int32_t share_target_ma(ch_id_t ch, int32_t total)
{
    if (ch >= CH_COUNT) return 0;
    const pd_node_t *n1 = pd_node_get(CH1);
    const pd_node_t *n2 = pd_node_get(CH2);
    if (sh.mode == SHARE_MODE_NORMAL || !n1->contract_valid || !n2->contract_valid ||
        n1->contract_ma == 0 || n2->contract_ma == 0)
        return total / 2;
    int32_t w = (ch == CH1) ? n1->contract_ma : n2->contract_ma;
    return total * w / ((int32_t)n1->contract_ma + n2->contract_ma);
}

uint16_t share_center_mv(void)
{
    return sh.v0;
}

bool share_anchor(ch_id_t ch)
{
    return ch < CH_COUNT && sh.state != SHARE_OFF && sh.anchor[ch];
}

int16_t share_offset_mv(ch_id_t ch)
{
    return (ch < CH_COUNT) ? sh.off[ch] : 0;
}

uint32_t share_adjust_count(void)
{
    return sh.adjustments;
}

void share_learn_start(void)
{
    for (uint8_t b = 0; b < SHARE_LEARN_BINS; b++) share_learn.bin[b] = (share_learn_bin_t){0};
    share_learn.active = 1;
    share_learn.result = 0xFF;
    LOG("SHARE learn start (%s)", share_mode_str(sh.mode));
}

void share_learn_stop(void)
{
    share_learn.active = 0;
}

bool share_learning(void)
{
    return share_learn.active;
}

// Least-squares fit y = a + k·x: x in units of 100mA (average total current per bin), y is the average offset difference per bin (mV).
// 32-bit integers suffice (x ≤ 120, |y| ≤ about 2500, ≤ 24 bins), avoiding linking 64-bit division
share_fit_t share_learn_save(void)
{
    share_learn_t *L = &share_learn;
    if (!L->active) return (share_fit_t)(L->result = SHARE_FIT_NOT_LEARNING);

    int32_t xs[SHARE_LEARN_BINS], ys[SHARE_LEARN_BINS];
    int32_t n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, xmin = 0, xmax = 0;
    for (uint8_t b = 0; b < SHARE_LEARN_BINS; b++)
    {
        const share_learn_bin_t *e = &L->bin[b];
        if (e->n < SHARE_LEARN_MIN_SAMPLES) continue;
        int32_t x = (e->sum_ma / e->n + 50) / 100, y = e->sum_mv / e->n;
        if (!n || x < xmin) xmin = x;
        if (!n || x > xmax) xmax = x;
        xs[n] = x;
        ys[n] = y;
        n++;
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    share_fit_t res = SHARE_FIT_OK;
    uint32_t fp1 = caps_fp(CH1), fp2 = caps_fp(CH2);
    if (n < SHARE_LEARN_MIN_BINS)                           res = SHARE_FIT_FEW_BINS;
    else if ((xmax - xmin) * 100 < SHARE_LEARN_MIN_SPAN_MA) res = SHARE_FIT_NARROW;
    else if (!fp1 || !fp2)                                  res = SHARE_FIT_NO_CAPS;
    L->result = (uint8_t)res;
    if (res != SHARE_FIT_OK) return res;

    int32_t num = n * sxy - sx * sy, den = n * sxx - sx * sx;
    int32_t k = num / den * 10 + num % den * 10 / den;      // mV/A
    int32_t a = (sy * 10 - k * sx) / (10 * n);
    int32_t r = 0;
    for (int32_t i = 0; i < n; i++)
    {
        int32_t e = ys[i] - (a + k * xs[i] / 10);
        if (e < 0) e = -e;
        if (e > r) r = e;
    }
    share_profile_t pf = {.fp = {fp1, fp2}, .v0 = sh.v0, .a_mv = (int16_t)a, .k_mv_per_a = (int16_t)k,
                          .resid_mv = (uint16_t)r, .bins = (uint8_t)n, .valid = 1};
    settings_set_profile(&pf);
    L->valid = 1;
    L->bins = (uint8_t)n;
    L->a_mv = pf.a_mv;
    L->k_mv_per_a = pf.k_mv_per_a;
    L->resid_mv = pf.resid_mv;
    L->v0 = pf.v0;
    L->active = 0;
    LOG("SHARE learn saved: a=%ldmV k=%ldmV/A resid=%ldmV bins=%ld v0=%u", a, k, r, n, sh.v0);
    return res;
}
