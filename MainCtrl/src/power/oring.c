#include "oring.h"

#include <stddef.h>
#include "config.h"
#include "pd/pd_node.h"
#include "power/meter.h"
#include "bsp/log.h"
#include "bsp/delay.h"
#include "bsp/resetlog.h"
#include "app/settings.h"
#include "power/share.h"
#include "app/stats.h"

static const uint16_t lm_en_pin[CH_COUNT] = {LM1_EN_GPIO_PIN, LM2_EN_GPIO_PIN};
#define LM_EN_PINS (LM1_EN_GPIO_PIN | LM2_EN_GPIO_PIN)

// Detection sampling: settling time + multiple consistent reads
#define DETECT_SAMPLES  8

static oring_hw_t  hw = ORING_HW_UNKNOWN;
static oring_hw_t  detect_result = ORING_HW_UNKNOWN;   // Raw detection result, for diagnostics
static bool        on_level_high;       // Output level when conducting

// Per-channel protection state; timer fields hold the time the condition started to hold, 0 means not timing
typedef struct prot_t
{
    bool        want_on;        // User intent: automatic recovery after a non-latching shutoff is based on this
    bool        latched;        // Latched fault, user must acknowledge to clear
    const char *trip;           // Reason for the most recent automatic shutoff
    uint16_t    ref_mv;         // Voltage reference: contract voltage or measured voltage at turn-on
    uint32_t    detach_seen;
    uint32_t    hard_reset_seen;

    uint32_t    t_ocp, t_rev, t_otp, t_vwin, t_isense, t_restore;

    uint32_t    trip_window_start;
    uint8_t     trips;          // Number of non-latching shutoffs within the window

    // Measured value of the most recent shutoff (kept after clearing the fault, for diagnostics)
    int32_t     trip_value;
    const char *trip_unit;      // "mA" / "mV" / "mC", NULL for event-type shutoffs
    uint32_t    trip_ms;

    bool        wait_partner;   // OCP XFER: wait for the other channel to turn on again before recovering
    uint32_t    on_ms;          // Time of the most recent turn-on
    uint32_t    t_leak, t_drop; // Timers for off leakage / abnormal conduction drop (placed at the end so diagnostic script offsets stay unchanged)
    bool        warm_hold;      // Conduction restored immediately after a warm reset: skip PD event-type checks until the PD link's first report
    uint32_t    t_vin;          // Input drop timer
    uint32_t    t_leak_lo;      // Off leakage (low threshold) timer
} prot_t;

static prot_t prot[CH_COUNT];

// Protection event log: the last PROT_EVENT_LOG_LEN shutoffs (time, channel, reason, value, latched or not), read via WCH-Link
typedef struct prot_event_t
{
    uint32_t    ms;
    const char *reason;
    int32_t     value;
    uint8_t     ch;
    uint8_t     latched;
    uint8_t     trips;
    uint8_t     pad;
} prot_event_t;

typedef struct prot_event_log_t
{
    uint32_t     magic;         // 0x50455654 "PEVT"
    uint32_t     count;
    prot_event_t e[PROT_EVENT_LOG_LEN];
} prot_event_log_t;

static prot_event_log_t prot_log = {.magic = 0x50455654};

static const char R_VSET[] = "VSET";     // Shutoff caused by voltage switching: not counted, no backoff
static const char R_REPLUG[] = "REPLUG"; // Software replug: not counted
static const char R_OFF_LEAK[] = "OFF LEAK";
static const char R_PATH_DROP[] = "PATH DROP";

// oring_init runs before delay_init, so a busy loop is used to wait for the external pull-up/down to settle (about 0.3ms at 144MHz)
static void settle_wait(void)
{
    for (volatile uint32_t i = 0; i < 20000; i++);
}

// Read both pins as floating inputs: all high → first version (pull-up), all low → revised version (pull-down), otherwise unknown
static oring_hw_t detect_hw(void)
{
    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = LM_EN_PINS;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(LM_EN_GPIO_PORT, &gpio);

    // First wait for the residual level on the EN line to bleed off before sampling (v0.0.103): if EN was driven before the reset, C30 (100nF) on the LTC4359 board via R12 (50k)
    // discharges with τ ≈ 5ms; originally it waited only 0.3ms and sampling took about 3ms in total, so it still read the pre-reset high level → misdetected as LM74700,
    // after which "on" actually pulled EN low and both channels stayed off (2026-10-26: a software reset 4.6s after flashing, under 5s so no warm reset path, detection ran again).
    // Wait about 36ms (> 7τ; the LM74700 board uses pull-ups, so waiting has no effect)
    for (uint8_t i = 0; i < 120; i++) settle_wait();

    uint8_t high = 0, low = 0;
    for (uint8_t i = 0; i < DETECT_SAMPLES; i++)
    {
        for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
        {
            if (GPIO_ReadInputDataBit(LM_EN_GPIO_PORT, lm_en_pin[ch]) == Bit_SET) high++;
            else low++;
        }
        settle_wait();
    }

    if (high == DETECT_SAMPLES * CH_COUNT) return ORING_HW_LM74700;
    if (low == DETECT_SAMPLES * CH_COUNT) return ORING_HW_LTC4359;
    return ORING_HW_UNKNOWN;
}

static void save_paths(void);
static void save_latch(void);

// Latch reason restored after a warm reset (the original string pointer is invalid across versions, so the text is copied)
static char latch_reason[CH_COUNT][RL_REASON_LEN];

static void path_set(ch_id_t ch, bool enable)
{
    if (hw == ORING_HW_UNKNOWN) return;     // Pins are floating inputs, stay off

    if (enable == on_level_high)
        GPIO_SetBits(LM_EN_GPIO_PORT, lm_en_pin[ch]);
    else
        GPIO_ResetBits(LM_EN_GPIO_PORT, lm_en_pin[ch]);
    save_paths();
}

// Called from the reset entry (bsp/startup_reset.S, before .data/.bss initialization): on a warm reset, immediately re-drive EN according to the reset record,
// before C30 discharges to the SHDN threshold. Uses only registers, constants and the fixed-address reset record; must not access .data/.bss
void oring_reset_hook(void)
{
    uint8_t mask, rhw;
    if (!resetlog_early_paths(PROT_WARM_MIN_UPTIME_MS, &mask, &rhw)) return;
    if (rhw != ORING_HW_LM74700 && rhw != ORING_HW_LTC4359) return;
    bool on_high = (rhw == ORING_HW_LTC4359);

    RCC_APB2PeriphClockCmd(LM_EN_GPIO_CLK, ENABLE);
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        bool on = (mask & (1u << ch)) != 0;
        if (on == on_high)
            GPIO_SetBits(LM_EN_GPIO_PORT, lm_en_pin[ch]);
        else
            GPIO_ResetBits(LM_EN_GPIO_PORT, lm_en_pin[ch]);
    }
    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = LM_EN_PINS;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(LM_EN_GPIO_PORT, &gpio);
}

void oring_init(void)
{
    RCC_APB2PeriphClockCmd(LM_EN_GPIO_CLK, ENABLE);

    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++) prot[ch] = (prot_t){0};

    // Warm reset: C30 on the EN line still holds the pre-reset level (R12·C30 ≈ 5ms); floating detection would discharge it below the SHDN threshold,
    // turning off a conducting path for about 20ms (2026-10-25 measured with a 1A load: VOUT 47.5V → < 8V).
    // Instead, immediately re-drive EN according to the board variant in the reset record, skipping detection; at power-up, detect as usual
    uint8_t early_mask = 0, early_hw = 0;
    bool early = resetlog_early_paths(PROT_WARM_MIN_UPTIME_MS, &early_mask, &early_hw) &&
                 (early_hw == ORING_HW_LM74700 || early_hw == ORING_HW_LTC4359);
    oring_hw_t detected = early ? (oring_hw_t)early_hw : detect_hw();

    // When polarity is forced: if detection clearly shows the other version (both channels agree), refuse, treating it as the wrong firmware flashed;
    // if the two channels disagree (usually a missing pull-up/down resistor on one channel), follow the forced setting
#if BOARD_ORING_POLARITY == ORING_POL_LM74700
    hw = (detected == ORING_HW_LTC4359) ? ORING_HW_UNKNOWN : ORING_HW_LM74700;
#elif BOARD_ORING_POLARITY == ORING_POL_LTC4359
    hw = (detected == ORING_HW_LM74700) ? ORING_HW_UNKNOWN : ORING_HW_LTC4359;
#else
    hw = detected;
#endif
    detect_result = detected;

    if (hw == ORING_HW_UNKNOWN) return;     // Stay as floating inputs

    on_level_high = (hw == ORING_HW_LTC4359);

    // First set the output latch to the target level (off; on a warm reset, paths that were conducting before the reset are on) and then switch to output, avoiding a glitch at the moment of switching
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        bool on = early && (early_mask & (1u << ch));
        if (on == on_level_high)
            GPIO_SetBits(LM_EN_GPIO_PORT, lm_en_pin[ch]);
        else
            GPIO_ResetBits(LM_EN_GPIO_PORT, lm_en_pin[ch]);
    }

    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = LM_EN_PINS;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(LM_EN_GPIO_PORT, &gpio);
}

oring_hw_t oring_hw(void)
{
    return hw;
}

oring_hw_t oring_hw_detected(void)
{
    return detect_result;
}

const char *oring_hw_str(oring_hw_t h)
{
    switch (h)
    {
        case ORING_HW_LM74700: return "REV1 LM74700";
        case ORING_HW_LTC4359: return "REV2 LTC4359";
        default:               return "UNKNOWN";
    }
}

bool oring_off_is_leaky(void)
{
    return hw == ORING_HW_LM74700;
}

bool oring_path_is_enabled(ch_id_t ch)
{
    if (ch >= CH_COUNT || hw == ORING_HW_UNKNOWN) return false;

    bool level_high = GPIO_ReadOutputDataBit(LM_EN_GPIO_PORT, lm_en_pin[ch]) == Bit_SET;
    return level_high == on_level_high;
}

// Write the conduction state and reference into the reset record (not cleared by software / watchdog reset); restored by oring_warm_start after a warm reset
static void save_paths(void)
{
    uint8_t mask = 0;
    uint16_t ref[CH_COUNT];
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        if (oring_path_is_enabled(ch)) mask |= (uint8_t)(1u << ch);
        ref[ch] = prot[ch].ref_mv;
    }
    resetlog_save_paths(mask, (uint8_t)hw, ref);
}

// Write the latch state and reason into the reset record: restored by oring_restore_latch after a watchdog / software reset.
// Before v0.0.86 the latch was only in RAM; on 2026-10-22 a CH1 PATH DROP latch was cleared by an IWDG reset and then turned on again by auto output
static void save_latch(void)
{
    uint8_t mask = 0;
    const char *reason[CH_COUNT];
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        if (prot[ch].latched) mask |= (uint8_t)(1u << ch);
        reason[ch] = prot[ch].trip ? prot[ch].trip : "LATCHED";
    }
    resetlog_save_latch(mask, reason);
}

bool oring_restore_latch(void)
{
    uint8_t mask;
    if (!resetlog_warm_latch(&mask, latch_reason)) return false;
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        if (!(mask & (1u << ch))) continue;
        prot_t *p = &prot[ch];
        p->latched = true;
        p->want_on = false;
        p->trip = latch_reason[ch][0] ? latch_reason[ch] : "LATCHED";
        path_set(ch, false);
    }
    save_latch();
    return true;
}

// On reset, EN is turned off by the pull-down and VOUT discharges through the bleeder (τ about 0.43s); the normal flow turns it on again only after about 1.4s,
// LTC4359 charges the gate slowly at 10µA, the MOSFET conducts linearly at about 30V differential to charge the output capacitors, and the inrush current makes the charger's
// input drop to 36V and disconnect (2026-10-21, measured twice). On a warm reset, conduction is restored directly from the record at the earliest stage of boot.
bool oring_warm_start(void)
{
    uint8_t mask;
    uint16_t ref[CH_COUNT];
    if (hw == ORING_HW_UNKNOWN || !resetlog_warm_paths(PROT_WARM_MIN_UPTIME_MS, &mask, ref)) return false;

    bool any = false;
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        if (!(mask & (1u << ch)) || !ref[ch]) continue;
        prot_t *p = &prot[ch];
        p->want_on = true;
        p->ref_mv = ref[ch];
        p->warm_hold = true;
        p->on_ms = millis();
        path_set(ch, true);
        any = true;
    }
    return any;
}

// Warm-reset hold: after the PD link's first report, end the hold using the event count at that time as the baseline; if still not online after PROT_WARM_HOLD_MS
// end the hold and hand over to the PD LINK LOST check. Returns true during the hold
static bool warm_hold(ch_id_t ch, uint32_t now_ms)
{
    prot_t *p = &prot[ch];
    if (!p->warm_hold) return false;

    const pd_node_t *n = pd_node_get(ch);
    if (n->link == PD_LINK_ONLINE && n->status_valid)
    {
        p->warm_hold = false;
        p->detach_seen = n->detach_count;
        p->hard_reset_seen = n->hard_reset_count;
        LOG("ORING CH%d warm hold end: PD online after %lums", ch + 1, (uint32_t)(now_ms - p->on_ms));
        return false;
    }
    if ((uint32_t)(now_ms - p->on_ms) < PROT_WARM_HOLD_MS) return true;
    p->warm_hold = false;
    return false;
}

// ============================================================================
// Protection checks
// ============================================================================

// Returns true after the condition has held for hold_ms; resets the timer when the condition does not hold
static bool persist(uint32_t *since, bool cond, uint32_t now_ms, uint32_t hold_ms)
{
    if (!cond)
    {
        *since = 0;
        return false;
    }
    if (*since == 0)
    {
        // 0 is reserved for "not timing"; cannot use now_ms | 1, because on even times since would be 1ms ahead and the difference would wrap to a huge value and hold immediately
        *since = now_ms ? now_ms : 1;
        return hold_ms == 0;
    }
    return (uint32_t)(now_ms - *since) >= hold_ms;
}

static bool in_window(int32_t ref_mv, int32_t mv)
{
    int32_t lo = ref_mv * (100 - PROT_VWIN_LOW_PCT) / 100 - PROT_VWIN_MARGIN_MV;
    int32_t hi = ref_mv * (100 + PROT_VWIN_HIGH_PCT) / 100 + PROT_VWIN_MARGIN_MV;
    return mv >= lo && mv <= hi && mv <= PROT_OVP_ABS_MV;
}

int32_t oring_ocp_limit_ma(ch_id_t ch)
{
    const pd_node_t *n = pd_node_get(ch);
    int32_t base = PROT_OCP_NO_CONTRACT_MA;
    if (n->contract_valid && n->contract_ma > 0) base = n->contract_ma;
    // Contract just became invalid (e.g. X035 sends Soft_Reset because a request was not Accepted; VBUS is unchanged during renegotiation): briefly keep using the old contract current,
    // otherwise the threshold drops sharply to the 3A level, and a channel carrying more than 3.5A during current sharing would be shut off by OCP
    else if (n->held_ma && (uint32_t)(millis() - n->lost_ms) < PROT_OCP_HELD_MS) base = n->held_ma;
    int32_t limit = base * (100 + PROT_OCP_MARGIN_PCT + 10 * settings_get()->ocp_level) / 100;
    if (limit > PROT_OCP_MAX_MA) limit = PROT_OCP_MAX_MA;
#if PROT_OCP_TEST_MA
    if (limit > PROT_OCP_TEST_MA) limit = PROT_OCP_TEST_MA;
#endif
    return limit;
}

// Turn-on preconditions (shared by user turn-on and automatic recovery); gives the voltage reference on success
static oring_deny_t check_enable(ch_id_t ch, uint16_t *ref_mv)
{
    if (hw == ORING_HW_UNKNOWN) return ORING_DENY_HW;
    if (prot[ch].latched) return ORING_DENY_FAULT;

    const pd_node_t *n = pd_node_get(ch);
    if (n->link != PD_LINK_ONLINE) return ORING_DENY_LINK;
    if (n->vset == PD_VSET_BUSY) return ORING_DENY_VSET;

    const meter_ch_t *m = meter_get(ch);
    if (!m->online)
    {
        if (PROT_REQUIRE_ISENSE) return ORING_DENY_ISENSE;
        *ref_mv = n->contract_valid ? n->contract_mv : 0;   // Degraded mode: no voltage check
        return ORING_OK;
    }

    int32_t v = m->vbus_mv_avg;
    if (v < PROT_VIN_MIN_MV || v > PROT_OVP_ABS_MV) return ORING_DENY_NO_VIN;

    // With a contract, the measured value must match it (the contract may be stale due to missed messages); without a contract (non-PD source), use the measured value as the reference
    if (n->contract_valid)
    {
        if (!in_window(n->contract_mv, v)) return ORING_DENY_VIN_MISMATCH;
        *ref_mv = n->contract_mv;
    }
    else
    {
        *ref_mv = (uint16_t)v;
    }
    return ORING_OK;
}

static void enable(ch_id_t ch, uint16_t ref_mv)
{
    prot_t *p = &prot[ch];
    const pd_node_t *n = pd_node_get(ch);

    // Record the current event count; any later change of the count is treated as unplug / Hard Reset
    p->detach_seen = n->detach_count;
    p->hard_reset_seen = n->hard_reset_count;
    p->ref_mv = ref_mv;
    p->t_ocp = p->t_rev = p->t_otp = p->t_vwin = p->t_isense = p->t_restore = p->t_vin = 0;
    p->wait_partner = false;
    p->on_ms = millis();
    path_set(ch, true);
}

// OCP delay: when current sharing is running, both channels conducting and the combined current not exceeding the sum of both limits, it is just uneven distribution --
// notify current sharing to transfer current urgently, and relax the delay to PROT_OCP_SHARE_DELAY_MS.
// Only when both channels are on adjustable PDOs (PPS / AVS): a fixed-PDO charger usually protects itself quickly on overcurrent (output collapses), relaxing would only let it fall over first
static bool adjustable(ch_id_t ch)
{
    pdo_kind_t k = pd_node_get(ch)->contract_kind;
    return k == PDO_KIND_PPS || k == PDO_KIND_SPR_AVS || k == PDO_KIND_EPR_AVS;
}

static uint32_t ocp_delay(ch_id_t ch, int32_t i, int32_t limit)
{
    ch_id_t o = (ch == CH1) ? CH2 : CH1;
    if (i <= limit || share_state() != SHARE_RUN || !oring_path_is_enabled(o)) return PROT_OCP_DELAY_MS;
    if (!adjustable(ch) || !adjustable(o)) return PROT_OCP_DELAY_MS;
    if (i + meter_get(o)->current_ma > limit + oring_ocp_limit_ma(o)) return PROT_OCP_DELAY_MS;
    share_ocp_assist(ch);
    return PROT_OCP_SHARE_DELAY_MS;
}

// The other channel is not sharing the load right now: path off, very small current (e.g. charger already disconnected, its input held up by back-feed from the output through the MOSFET) or just shut off
static bool partner_down(ch_id_t ch, uint32_t now_ms)
{
    ch_id_t o = (ch == CH1) ? CH2 : CH1;
    return !oring_path_is_enabled(o) || meter_get(o)->current_ma < PROT_XFER_PARTNER_MA ||
           (prot[o].trip && (uint32_t)(now_ms - prot[o].trip_ms) < PROT_XFER_RECENT_MS);
}

static void trip(ch_id_t ch, const char *reason, bool latch, int32_t value, const char *unit, uint32_t now_ms)
{
    prot_t *p = &prot[ch];
    path_set(ch, false);
    p->trip = reason;
    p->trip_value = value;
    p->trip_unit = unit;
    p->trip_ms = now_ms;

    if (!latch && reason != R_VSET && reason != R_REPLUG)
    {
        // Repeated shutoffs (e.g. voltage oscillation) turn into a latch, avoiding repeated on/off cycling
        if (p->trips == 0 || (uint32_t)(now_ms - p->trip_window_start) > PROT_TRIP_WINDOW_MS)
        {
            p->trip_window_start = now_ms;
            p->trips = 0;
        }
        if (++p->trips >= PROT_TRIP_MAX) latch = true;
    }
    if (latch)
    {
        p->latched = true;
        p->want_on = false;
    }
    save_latch();

    if (unit)
        LOG("ORING CH%d OFF: %s %ld%s ref=%umV%s", ch + 1, reason, value, unit, p->ref_mv,
            latch ? " (latched)" : "");
    else
        LOG("ORING CH%d OFF: %s%s", ch + 1, reason, latch ? " (latched)" : "");
    stats_on_trip(ch, reason, latch, value, unit, now_ms);

    prot_event_t *e = &prot_log.e[prot_log.count % PROT_EVENT_LOG_LEN];
    *e = (prot_event_t){.ms = now_ms, .reason = reason, .value = value, .ch = (uint8_t)ch,
                        .latched = latch, .trips = p->trips};
    prot_log.count++;
}

// Paths already on: event interlocks and measurement protections; returns true if it has been shut off
static bool protect(ch_id_t ch, uint32_t now_ms)
{
    prot_t *p = &prot[ch];
    const pd_node_t *n = pd_node_get(ch);
    const meter_ch_t *m = meter_get(ch);

    const char *why = NULL;
    bool latch = false;
    int32_t val = 0;
    const char *unit = NULL;

    bool pd = !warm_hold(ch, now_ms);    // During the warm-reset hold, only measurement-based protections are applied

    if (pd && n->link != PD_LINK_ONLINE)                 why = "PD LINK LOST";
    else if (pd && n->detach_count != p->detach_seen)    why = "DETACH";
    else if (pd && n->hard_reset_count != p->hard_reset_seen)
    {
        // Value: high 16 bits = 3 received (source initiated) / 4 sent (X035 initiated), low 16 bits = X035 event parameter
        why = "HARD RESET"; val = ((int32_t)n->last_hr_code << 16) | n->last_hr_arg;
    }
    else if (pd && n->vset == PD_VSET_BUSY)              why = R_VSET;
    else if (pd && p->ref_mv && n->contract_valid && !in_window(p->ref_mv, n->contract_mv))
    {
        why = "PD CONTRACT"; val = n->contract_mv; unit = "mV";
    }
    else if (!m->online)
    {
        if (PROT_REQUIRE_ISENSE && persist(&p->t_isense, true, now_ms, PROT_ISENSE_LOST_MS))
            why = "I-SENSE LOST";
    }
    else
    {
        p->t_isense = 0;
        int32_t i = m->current_ma;
        int32_t limit = oring_ocp_limit_ma(ch);

        if (p->ref_mv && persist(&p->t_vin, m->vbus_mv < (int32_t)p->ref_mv * PROT_VIN_LOST_PCT / 100,
                                 now_ms, PROT_VIN_LOST_MS))
        {
            why = "VIN LOST"; val = m->vbus_mv; unit = "mV";
        }
        else if (i >= PROT_SCP_MA)
        {
            why = "SCP"; latch = true; val = i; unit = "mA";
        }
        else if (persist(&p->t_ocp, i > limit, now_ms, ocp_delay(ch, i, limit)))
        {
            // The other channel dropped out and the whole load moved to this channel: do not latch, wait for the other channel to recover
            bool xfer = partner_down(ch, now_ms);
            why = xfer ? "OCP XFER" : "OCP"; latch = !xfer; val = i; unit = "mA";
            p->wait_partner = xfer;
        }
        else if (persist(&p->t_rev, i < PROT_REVERSE_MA, now_ms, PROT_REVERSE_DELAY_MS))
        {
            why = "REVERSE"; latch = true; val = i; unit = "mA";
        }
        else if (persist(&p->t_otp, m->temp_mc > PROT_OTP_MC, now_ms, PROT_OTP_DELAY_MS))
        {
            why = "OTP"; latch = true; val = m->temp_mc; unit = "mC";
        }
        else if (m->vbus_mv > PROT_OVP_ABS_MV)
        {
            why = "OVP"; latch = true; val = m->vbus_mv; unit = "mV";
        }
        else if (p->ref_mv && persist(&p->t_vwin, !in_window(p->ref_mv, m->vbus_mv), now_ms, PROT_VWIN_DELAY_MS))
        {
            why = "VOLTAGE"; val = m->vbus_mv; unit = "mV";
        }
    }

    if (!why) return false;
    trip(ch, why, latch, val, unit, now_ms);
    return true;
}

// After a non-latching shutoff (or after oring_arm at power-up): once the contract voltage matches the reference and other conditions are met, turn on after they hold for a while
static void try_restore(ch_id_t ch, uint32_t now_ms)
{
    prot_t *p = &prot[ch];
    const pd_node_t *n = pd_node_get(ch);
    uint16_t ref;

    // Backoff: after the n-th non-latching shutoff, wait at least backoff[n-1]
    static const uint16_t backoff[] = PROT_BACKOFF_MS;
    if (p->trip && p->trips && (uint32_t)(now_ms - p->trip_ms) <
        backoff[(p->trips <= sizeof(backoff) / sizeof(backoff[0]) ? p->trips : sizeof(backoff) / sizeof(backoff[0])) - 1])
    {
        p->t_restore = 0;
        return;
    }

    bool ok = p->ref_mv && n->contract_valid && in_window(p->ref_mv, n->contract_mv) &&
              check_enable(ch, &ref) == ORING_OK;
    if (ok && p->wait_partner)
    {
        // OCP XFER: the other channel has been turned on again after this channel shut off; or the retry time has passed; when both are waiting, CH1 goes first
        ch_id_t o = (ch == CH1) ? CH2 : CH1;
        ok = (oring_path_is_enabled(o) && (int32_t)(prot[o].on_ms - p->trip_ms) > 0) ||
             (uint32_t)(now_ms - p->trip_ms) > PROT_XFER_RETRY_MS ||
             (prot[o].wait_partner && ch == CH1);
    }
    if (!persist(&p->t_restore, ok, now_ms, PROT_RESTORE_MS)) return;

    enable(ch, p->ref_mv);
    LOG("ORING CH%d ON: auto %s, ref=%umV", ch + 1, p->trip ? p->trip : "boot", p->ref_mv);
}

// ============================================================================
// Public interface
// ============================================================================

oring_deny_t oring_replug(ch_id_t ch)
{
    if (ch >= CH_COUNT) return ORING_OK;
    prot_t *p = &prot[ch];
    const pd_node_t *n = pd_node_get(ch);

    if (n->link != PD_LINK_ONLINE) return ORING_DENY_LINK;
    if (n->fw_major < 2 || (n->fw_major == 2 && n->fw_minor < 2)) return ORING_DENY_FW;
    if (meter_get((ch == CH1) ? CH2 : CH1)->vbus_mv < PROT_REPLUG_PARTNER_MV) return ORING_DENY_NO_PARTNER;

    bool want = p->want_on || oring_path_is_enabled(ch);
    p->latched = false;
    trip(ch, R_REPLUG, false, 0, NULL, millis());
    p->want_on = want;
    if (!pd_node_error_recovery(ch, PROT_REPLUG_MS)) return ORING_DENY_LINK;
    LOG("ORING CH%d REPLUG %ums", ch + 1, PROT_REPLUG_MS);
    return ORING_OK;
}

oring_deny_t oring_request(ch_id_t ch, bool on)
{
    if (ch >= CH_COUNT) return ORING_OK;
    prot_t *p = &prot[ch];

    if (!on)
    {
        bool was = p->want_on || oring_path_is_enabled(ch);
        p->want_on = false;
        path_set(ch, false);
        if (was) LOG("ORING CH%d OFF: user", ch + 1);
        return ORING_OK;
    }

    uint16_t ref = 0;
    oring_deny_t deny = check_enable(ch, &ref);
    if (deny != ORING_OK)
    {
        LOG("ORING CH%d ON denied: %s", ch + 1, oring_deny_str(deny));
        return deny;
    }

    p->want_on = true;
    p->trip = NULL;
    p->trips = 0;
    enable(ch, ref);
    LOG("ORING CH%d ON: user ref=%umV ocp=%ldmA%s", ch + 1, ref, oring_ocp_limit_ma(ch),
        meter_get(ch)->online ? "" : " (NO I-SENSE)");
    return ORING_OK;
}

void oring_arm(ch_id_t ch, uint16_t ref_mv)
{
    if (ch >= CH_COUNT || ref_mv == 0 || prot[ch].latched) return;
    prot_t *p = &prot[ch];
    p->want_on = true;
    p->ref_mv = ref_mv;
    p->trip = NULL;
    p->trips = 0;
    p->t_restore = 0;
    LOG("ORING CH%d armed: auto on at %umV", ch + 1, ref_mv);
}

void oring_all_off(const char *reason)
{
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        prot[ch].want_on = false;
        path_set(ch, false);
    }
    LOG("ORING ALL OFF: %s", reason);
}

// Off leakage: on the LTC4359 board the back-to-back MOSFETs should be fully open after shutoff; current still flowing while off (either direction) = one MOSFET is shorted (visible only under load)
static void check_off_leak(ch_id_t ch, uint32_t now_ms)
{
    prot_t *p = &prot[ch];
    const meter_ch_t *m = meter_get(ch);
    // Take the smaller of the average and the instantaneous value: after shutting off under load the average (low-pass about 100ms) takes more than 300ms to fall below the threshold,
    // measured at 5.7A: after an OCP shutoff OFF LEAK was falsely detected at 322mA; with a real leak both stay high
    int32_t i = m->current_ma_avg, ia = i < 0 ? -i : i;
    int32_t ir = m->current_ma < 0 ? -m->current_ma : m->current_ma;
    if (ir < ia)
    {
        ia = ir;
        i = m->current_ma;
    }
    bool ok = hw == ORING_HW_LTC4359 && m->online;
    // High threshold for a fast decision; low threshold is slower: the current of a healthy path that is off should be close to 0 (INA offset only a few mA),
    // on 2026-10-22 when Q3 shorted, at no load there was only the 48mA bleeder current, which the 150mA threshold could not catch
    bool hi = persist(&p->t_leak, ok && ia > PROT_OFF_LEAK_MA, now_ms, PROT_OFF_LEAK_MS);
    bool lo = persist(&p->t_leak_lo, ok && ia > PROT_OFF_LEAK_LOW_MA, now_ms, PROT_OFF_LEAK_LOW_MS);
    if (!hi && !lo) return;
    if (p->latched && p->trip == R_OFF_LEAK) return;   // Already recorded; if it still leaks after the fault is cleared it will be recorded again
    trip(ch, R_OFF_LEAK, true, i, "mA", now_ms);
}

// Abnormal conduction drop: both channels conducting, this channel's input is more than PROT_DROP_DV_MV higher than the other's, a normal ideal diode would have conducted long ago,
// yet it carries less than PROT_DROP_MIN_PCT% = this channel's MOSFET is not being driven on and current flows through the body diode (0.4~0.8V drop, heating)
static void check_path_drop(ch_id_t ch, uint32_t now_ms)
{
    prot_t *p = &prot[ch];
    ch_id_t o = (ch == CH1) ? CH2 : CH1;
    const meter_ch_t *m = meter_get(ch);
    const meter_ch_t *mo = meter_get(o);
    int32_t total = m->current_ma_avg + mo->current_ma_avg;
    int32_t dv = m->vbus_mv_avg - mo->vbus_mv_avg;
    bool bad = oring_path_is_enabled(o) && m->online && mo->online && total >= PROT_DROP_MIN_TOTAL_MA &&
               dv >= PROT_DROP_DV_MV && m->current_ma_avg * 100 < total * PROT_DROP_MIN_PCT;
    if (persist(&p->t_drop, bad, now_ms, PROT_DROP_MS)) trip(ch, R_PATH_DROP, true, dv, "mV", now_ms);
}

void oring_process(uint32_t now_ms)
{
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        prot_t *p = &prot[ch];

        if (oring_path_is_enabled(ch))
        {
            p->t_leak = p->t_leak_lo = 0;
            if (!protect(ch, now_ms)) check_path_drop(ch, now_ms);
            continue;
        }
        p->t_drop = 0;
        check_off_leak(ch, now_ms);

        if (p->want_on && !p->latched)
            try_restore(ch, now_ms);
        else
            p->t_restore = 0;
    }
}

bool oring_overload(void)
{
    int32_t i = 0, lim = 0;
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        if (!oring_path_is_enabled(ch)) continue;
        i += meter_get(ch)->current_ma_avg;
        lim += pd_node_get(ch)->contract_ma;
    }
    return lim && i * 100 > lim * PROT_OVERLOAD_PCT;
}

const char *oring_last_trip(ch_id_t ch)
{
    return (ch < CH_COUNT) ? prot[ch].trip : NULL;
}

bool oring_last_trip_value(ch_id_t ch, int32_t *value, const char **unit, uint32_t *ms)
{
    if (ch >= CH_COUNT || !prot[ch].trip) return false;
    if (value) *value = prot[ch].trip_value;
    if (unit) *unit = prot[ch].trip_unit;
    if (ms) *ms = prot[ch].trip_ms;
    return true;
}

bool oring_wants_on(ch_id_t ch)
{
    return ch < CH_COUNT && prot[ch].want_on;
}

bool oring_fault_latched(ch_id_t ch)
{
    return ch < CH_COUNT && prot[ch].latched;
}

void oring_clear_fault(ch_id_t ch)
{
    if (ch >= CH_COUNT) return;
    prot_t *p = &prot[ch];
    LOG("ORING CH%d fault cleared (%s)", ch + 1, p->trip ? p->trip : "-");
    p->latched = false;
    p->trip = NULL;
    p->trips = 0;
    p->wait_partner = false;
    save_latch();
}

bool oring_path_down(ch_id_t ch)
{
    return ch < CH_COUNT && !oring_path_is_enabled(ch) && (prot[ch].want_on || prot[ch].latched);
}

const char *oring_deny_str(oring_deny_t deny)
{
    switch (deny)
    {
        case ORING_DENY_LINK:         return "PD LINK OFFLINE";
        case ORING_DENY_HW:           return "HW REV UNKNOWN";
        case ORING_DENY_FAULT:        return "FAULT LATCHED";
        case ORING_DENY_ISENSE:       return "NO I-SENSE";
        case ORING_DENY_NO_VIN:       return "VIN OUT OF RANGE";
        case ORING_DENY_VIN_MISMATCH: return "VIN != PD CONTRACT";
        case ORING_DENY_VSET:         return "VOLTAGE SWITCHING";
        case ORING_DENY_NO_PARTNER:   return "OTHER CH NO POWER";
        case ORING_DENY_FW:           return "X035 FW < 2.2";
        default:                      return "OK";
    }
}
