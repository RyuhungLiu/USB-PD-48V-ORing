#include "usbpd_pe.h"

#include <string.h>
#include "config.h"
#include "nv.h"
#include "sys_time.h"
#include "usbpd_def.h"
#include "usbpd_prl.h"
#include "vbus.h"

// Sink_Capabilities of this Sink: vSafe5V 3A + PPS 3.3..21V 5A
#define SNK_PDO_FIXED_5V        ((1u << 28) | (100u << 10) | 300u)
#define SNK_PDO_PPS             ((3u << 30) | (210u << 17) | (33u << 8) | 100u)

static pe_status_t st;

static struct
{
    uint32_t    now;
    uint32_t    t_entry;            // time of entering the current state
    bool        entry;              // entry action of the current state not yet executed
    bool        attached;
    bool        resume;             // in the keep-contract-at-boot flow
    uint8_t     tries;              // Get_Source_Cap / EPR_KeepAlive attempt count
    bool        soft_reset_tried;   // Soft_Reset already sent on SinkWaitCap timeout
    uint8_t     hard_reset_count;   // HardResetCounter (2.2.3+: cleared to zero once a contract is obtained)
    bool        no_accept_sr;       // Soft_Reset already sent because a request got no Accept (Hard Reset only on another timeout; cleared once a contract is obtained)
    uint8_t     sr_arg;             // SOFT_RESET event arg for the next Send_Soft_Reset (0 = default 1)
    bool        hard_reset_req;     // requested by V203
    uint8_t     hr_reason;          // reason for the Hard Reset (link_hr_reason_t)
    uint16_t    hr_waited;          // milliseconds already spent in the previous state when it was issued
    bool        target_dirty;       // new target pending evaluation
    bool        force_request;      // target issued by the main controller: request again even if identical to the current contract (so the main controller gets a PS_RDY confirmation)
    bool        caps_fresh;         // source capabilities just received: spec requires a Request reply (otherwise the partner's SenderResponseTimer times out and it sends Hard Reset)
    bool        epr_auto_failed;    // automatic EPR entry failed (e.g. cable not supported): no more automatic attempts for this attach
    bool        epr_entry_pending;  // obtain an SPR contract first, then enter EPR
    bool        epr_failed;         // EPR entry failed for this target, do not retry
    uint32_t    retry_at;           // SinkRequestTimer after Wait
    uint32_t    pps_at;             // SinkPPSPeriodicTimer
    uint32_t    keepalive_at;       // SinkEPRKeepAliveTimer
    bool        keepalive_wait;     // EPR_KeepAlive sent, waiting for Ack
    uint32_t    keepalive_deadline;
    uint8_t     keepalive_miss;
    uint32_t    rp_at;
    uint32_t    txng_since;         // time Rp = SinkTxNG started
    uint32_t    hr_until;           // Hard Reset recovery period
    bool        hr_vbus_dropped;    // VBUS has dropped near vSafe0V during the recovery period
    pd_select_t sel;                // PDO being requested
} pe;

static void bump(void)
{
    st.rev++;
}

static void set_state(pe_state_t s)
{
    st.state = s;
    pe.t_entry = pe.now;
    pe.entry = true;
    st.busy = !(s == PE_SNK_Ready || s == PE_SNK_Detached || s == PE_SNK_Wait_for_Capabilities ||
                s == PE_SNK_Discovery);
    bump();
}

static bool elapsed(uint32_t ms)
{
    return (uint32_t)(pe.now - pe.t_entry) >= ms;
}

// PD 3.x collision avoidance: the Sink only initiates an AMS when Rp = SinkTxOk (3.0A); if SinkTxNG lasts over 100ms, send anyway
static bool sink_tx_ok(void)
{
    if (st.spec_rev < PD_REV_30) return true;
    if (phy_rp_level() != PHY_RP_1A5)
    {
        pe.txng_since = 0;
        return true;
    }
    if (!pe.txng_since) pe.txng_since = pe.now ? pe.now : 1;
    return (uint32_t)(pe.now - pe.txng_since) >= 100;
}

static void clear_contract(void)
{
    st.explicit_contract = false;
    st.epr_mode = false;
    memset(&st.contract, 0, sizeof(st.contract));
    pe.keepalive_wait = false;
    bump();
}

static void store_caps(const uint32_t *caps, uint8_t n, bool epr)
{
    if (n > PE_MAX_CAPS) n = PE_MAX_CAPS;
    memset(st.caps, 0, sizeof(st.caps));
    memcpy(st.caps, caps, 4u * n);
    st.n_caps = n;
    st.caps_epr = epr;
    // EPR Mode Capable / revision of PDO1 (vSafe5V fixed PDO)
    st.src_epr_capable = (caps[0] >> 30) == 0 && (caps[0] & PD_PDO_FIXED_EPR_MODE_CAPABLE);
    st.caps_seq++;
    pe.caps_fresh = true;
    bump();
}

// Source_Capabilities / EPR_Source_Capabilities; returns true = it is a source capabilities message
static bool take_caps(const prl_msg_t *m)
{
    if (!m->extended && m->type == PD_DATA_Source_Capabilities && m->num_do)
    {
        prl_set_spec_rev(m->spec_rev);
        st.spec_rev = prl_spec_rev();
        store_caps(m->obj, m->num_do, false);
        // Receiving SPR source capabilities means we are not in EPR mode (after Soft Reset / EPR exit)
        st.epr_mode = false;
        return true;
    }
    if (m->extended && m->type == PD_EXT_EPR_Source_Capabilities && m->ext_size >= 4)
    {
        uint32_t caps[PE_MAX_CAPS];
        uint8_t n = (uint8_t)(m->ext_size / 4);
        if (n > PE_MAX_CAPS) n = PE_MAX_CAPS;
        for (uint8_t i = 0; i < n; i++)
            caps[i] = (uint32_t)m->ext[4 * i] | ((uint32_t)m->ext[4 * i + 1] << 8) |
                      ((uint32_t)m->ext[4 * i + 2] << 16) | ((uint32_t)m->ext[4 * i + 3] << 24);
        store_caps(caps, n, true);
        st.epr_mode = true;
        return true;
    }
    return false;
}

static bool is_ctrl(const prl_msg_t *m, pd_ctrl_msg_t t)
{
    return !m->extended && m->num_do == 0 && m->type == t;
}

static bool is_epr_mode(const prl_msg_t *m, pd_epr_mode_action_t a)
{
    return !m->extended && m->num_do >= 1 && m->type == PD_DATA_EPR_Mode && PD_EPR_MODE_ACTION(m->obj[0]) == a;
}

static bool is_ext_ctrl(const prl_msg_t *m, pd_ext_ctrl_t t)
{
    return m->extended && m->type == PD_EXT_Extended_Control && m->ext_size >= 1 && m->ext[0] == t;
}

static void send_not_supported(void)
{
    prl_tx_ctrl(st.spec_rev >= PD_REV_30 ? PD_CTRL_Not_Supported : PD_CTRL_Reject);
}

// ---------------------------------------------------------------------------
// PDO selection: target -> PDO; marks epr_entry_pending when EPR is needed
// Returns false = no Request needs to be (or can be) sent
// ---------------------------------------------------------------------------
static bool evaluate(void)
{
    const pe_target_t *t = &st.target;
    st.target_unmet = false;
    pe.epr_entry_pending = false;

    // EPR needed: target above 20V, charger supports EPR, not yet in EPR, allowed
    bool want_epr = t->mv > 20000 && !st.epr_mode && st.src_epr_capable &&
                    !(t->flags & PE_TARGET_NO_EPR) && !pe.epr_failed;
    if (want_epr)
    {
        pe.epr_entry_pending = true;
        // Already have an SPR contract: enter EPR directly (when source capabilities were just received, must first reply with Request keeping the current voltage)
        if (st.explicit_contract && !pe.caps_fresh) return false;
        if (st.explicit_contract)
            return pd_select_nearest(st.caps, st.n_caps, st.contract.mv ? st.contract.mv : vbus_mv(), &pe.sel);
        // No contract yet: first obtain an SPR contract (the fixed PDO closest to the target, i.e. the highest SPR fixed PDO)
        return pd_select_nearest(st.caps, st.n_caps, t->mv, &pe.sel);
    }

    if (pe.resume && !nv_target() && !t->mv)
    {
        // Keeping the contract with no stored target: keep the current voltage
        if (pd_select_nearest(st.caps, st.n_caps, vbus_mv(), &pe.sel)) return true;
    }

    if (pd_select(st.caps, st.n_caps, t->mode, t->mv ? t->mv : 5000, t->ma, &pe.sel))
    {
        // Same as the current contract: do not request again (except for targets issued by the main controller and freshly received source capabilities; periodic PPS requests are handled separately)
        if (!pe.force_request && !pe.caps_fresh && st.explicit_contract && pe.sel.pos == st.contract.pos &&
            pe.sel.mv == st.contract.mv && pe.sel.ma == st.contract.ma)
            return false;
        return true;
    }

    st.target_unmet = true;
    st.last_result = PE_RESULT_NO_MATCH;
    bump();
    // With no contract yet, at least obtain vSafe5V; when source capabilities were just received, keep the current voltage (a Request reply is mandatory)
    if (!st.explicit_contract) return pd_select(st.caps, st.n_caps, PD_SELECT_FIXED_FIRST, 5000, 0, &pe.sel);
    if (pe.caps_fresh)
        return pd_select_nearest(st.caps, st.n_caps, st.contract.mv ? st.contract.mv : vbus_mv(), &pe.sel);
    return false;
}

// ---------------------------------------------------------------------------
// State handling
// ---------------------------------------------------------------------------
static void enter_hard_reset(link_hr_reason_t reason)
{
    uint32_t ms = pe.now - pe.t_entry;
    pe.hr_reason = (uint8_t)reason;
    pe.hr_waited = (uint16_t)(ms > 0xFFFF ? 0xFFFF : ms);
    set_state(PE_SNK_Hard_Reset);
}

static void epr_entry_failed(uint8_t reason)
{
    pe.epr_failed = true;
    pe.epr_auto_failed = true;
    pe.epr_entry_pending = false;
    st.epr_fail_reason = reason;
    st.last_result = PE_RESULT_EPR_FAILED;
    link_send_event(LINK_EVENT_EPR_ENTER_FAILED, reason);
    set_state(PE_SNK_Ready);
}

static void after_evaluate(void)
{
    if (evaluate()) set_state(PE_SNK_Select_Capability);
    else if (pe.epr_entry_pending && st.explicit_contract) set_state(PE_SNK_Send_EPR_Mode_Entry);
    else if (st.explicit_contract) set_state(PE_SNK_Ready);
    else set_state(PE_SNK_Wait_for_Capabilities);
}

static void on_contract(void)
{
    // Both sides have communicated normally and completed a contract: clear HardResetCounter to zero (otherwise, after two accumulated, a further timeout would only silently wait for source capabilities,
    // and in EPR keep-alive would also stop; the partner then sends Hard Reset back after about 0.8s)
    pe.hard_reset_count = 0;
    pe.no_accept_sr = false;
    st.explicit_contract = true;
    st.contract.pos = pe.sel.pos;
    st.contract.kind = pe.sel.kind;
    st.contract.mv = pe.sel.mv;
    st.contract.ma = pe.sel.ma;
    st.contract_seq++;
    st.last_result = st.target_unmet ? PE_RESULT_NO_MATCH : PE_RESULT_OK;
    pe.pps_at = pe.now + PD_tPPSRequest;
    if (pe.resume)
    {
        pe.resume = false;
        link_send_event(LINK_EVENT_RESUME_OK, st.contract.mv);
    }
    link_send_event(LINK_EVENT_CONTRACT, st.contract.mv);
    bump();
}

static void run_state(const prl_msg_t *m)
{
    switch (st.state)
    {
        case PE_SNK_Startup:
            prl_reset();
            clear_contract();
            if (pe.resume)
            {
                pe.tries = 0;
                // Keep contract at boot: first try EPR keep-alive (with Auto EPR, 20V may also be in EPR mode); if the partner does not support it, fetch the SPR source capabilities
                set_state(PE_SNK_EPR_Keepalive);
                st.explicit_contract = true;    // the partner is still in a contract (content unknown)
                break;
            }
            set_state(PE_SNK_Discovery);
            break;

        case PE_SNK_Discovery:
            if (vbus_mv() >= CFG_VBUS_PRESENT_MV) set_state(PE_SNK_Wait_for_Capabilities);
            break;

        case PE_SNK_Wait_for_Capabilities:
            if (pe.entry) pe.entry = false;
            if (m && take_caps(m))
            {
                pe.soft_reset_tried = false;
                set_state(PE_SNK_Evaluate_Capability);
                break;
            }
            if (elapsed(PD_tTypeCSinkWaitCap) && pe.hard_reset_count < PD_nHardResetCount)
            {
                if (!pe.soft_reset_tried && vbus_mv() >= CFG_VBUS_PRESENT_MV)
                {
                    pe.soft_reset_tried = true;
                    set_state(PE_Send_Soft_Reset);
                }
                else
                {
                    enter_hard_reset(LINK_HR_WAIT_CAP);
                }
            }
            break;

        case PE_SNK_Evaluate_Capability:
            pe.force_request = pe.target_dirty;
            pe.target_dirty = false;
            after_evaluate();
            pe.force_request = false;
            pe.caps_fresh = false;
            break;

        case PE_SNK_Select_Capability:
            if (pe.entry)
            {
                pe.entry = false;
                bool ok;
                if (st.epr_mode)
                {
                    uint32_t obj[2] = {pe.sel.rdo, pe.sel.pdo};
                    ok = prl_tx_data(PD_DATA_EPR_Request, obj, 2);
                }
                else
                {
                    ok = prl_tx_data(PD_DATA_Request, &pe.sel.rdo, 1);
                }
                if (!ok) set_state(PE_Send_Soft_Reset);
                break;
            }
            if (m && is_ctrl(m, PD_CTRL_Accept))
            {
                uint32_t late = pe.now - pe.t_entry;
                if (late > PD_tSenderResponse) link_send_event(LINK_EVENT_ACCEPT_LATE, (uint16_t)late);
                set_state(PE_SNK_Transition_Sink);
            }
            else if (m && is_ctrl(m, PD_CTRL_Reject) && st.epr_mode &&
                     (pe.sel.kind == PD_PDO_SPR_PPS || pe.sel.kind == PD_PDO_SPR_AVS) &&
                     st.contract.kind == PD_PDO_FIXED && st.contract.mv <= 20000)
            {
                // Some chargers do not accept SPR adjustable PDOs while in EPR mode: exit EPR, fetch the SPR source capabilities and request again
                pe.target_dirty = true;
                set_state(PE_SNK_Send_EPR_Mode_Exit);
            }
            else if (m && (is_ctrl(m, PD_CTRL_Reject) || is_ctrl(m, PD_CTRL_Wait)))
            {
                bool wait = is_ctrl(m, PD_CTRL_Wait);
                st.last_result = wait ? PE_RESULT_WAIT : PE_RESULT_REJECT;
                link_send_event(wait ? LINK_EVENT_WAIT : LINK_EVENT_REJECT, pe.sel.mv);
                if (wait) pe.retry_at = pe.now + PD_tSinkRequest;
                set_state(st.explicit_contract ? PE_SNK_Ready : PE_SNK_Wait_for_Capabilities);
            }
            else if (m && take_caps(m))
            {
                set_state(PE_SNK_Evaluate_Capability);
            }
            else if (elapsed(CFG_ACCEPT_TIMEOUT_MS))
            {
                st.last_result = PE_RESULT_TIMEOUT;
                // 2.2.3+: Soft_Reset first (VBUS is not interrupted; after getting source capabilities, request again per target); Hard Reset only on another timeout.
                // Hard Reset drops VBUS to 0; when current balancing carries a heavy load the other channel cannot hold it alone and both go down
                if (!pe.no_accept_sr)
                {
                    pe.no_accept_sr = true;
                    pe.sr_arg = 2;
                    set_state(PE_Send_Soft_Reset);
                }
                else
                {
                    enter_hard_reset(LINK_HR_NO_ACCEPT);
                }
            }
            break;

        case PE_SNK_Transition_Sink:
            if (m && is_ctrl(m, PD_CTRL_PS_RDY))
            {
                on_contract();
                set_state(PE_SNK_Ready);
            }
            else if (elapsed(st.epr_mode ? PD_tPSTransition_EPR : PD_tPSTransition_SPR))
            {
                st.last_result = PE_RESULT_TIMEOUT;
                enter_hard_reset(LINK_HR_NO_PS_RDY);
            }
            break;

        case PE_SNK_Ready:
            if (pe.entry)
            {
                pe.entry = false;
                if (pe.epr_entry_pending && !st.epr_mode)
                {
                    set_state(PE_SNK_Send_EPR_Mode_Entry);
                    break;
                }
                if (st.epr_mode && !pe.keepalive_at) pe.keepalive_at = pe.now + PD_tSinkEPRKeepAlive;
            }
            if (m)
            {
                if (take_caps(m))
                {
                    set_state(PE_SNK_Evaluate_Capability);
                }
                else if (is_ctrl(m, PD_CTRL_Get_Sink_Cap))
                {
                    uint32_t snk[2] = {SNK_PDO_FIXED_5V, SNK_PDO_PPS};
                    prl_tx_data(PD_DATA_Sink_Capabilities, snk, 2);
                }
                else if (is_ext_ctrl(m, PD_EXT_CTRL_EPR_KeepAlive_Ack))
                {
                    pe.keepalive_wait = false;
                    pe.keepalive_miss = 0;
                }
                else if (is_epr_mode(m, PD_EPR_MODE_Exit))
                {
                    // The partner exits EPR and will then send SPR source capabilities
                    st.epr_mode = false;
                    bump();
                }
                else if (!m->extended && m->num_do == 0 &&
                         (m->type == PD_CTRL_Ping || m->type == PD_CTRL_GoodCRC || m->type == PD_CTRL_Accept ||
                          m->type == PD_CTRL_Reject || m->type == PD_CTRL_Not_Supported || m->type == PD_CTRL_PS_RDY))
                {
                    // No response needed
                }
                else if (!m->extended && m->type == PD_DATA_Vendor_Defined && m->num_do >= 1)
                {
                    // Structured VDM request (Discover Identity etc.): not supported
                    if ((m->obj[0] & (1u << 15)) && ((m->obj[0] >> 6) & 3) == 0) send_not_supported();
                }
                else if (!m->extended && m->type == PD_DATA_Alert)
                {
                    // Not handled
                }
                else
                {
                    send_not_supported();
                }
                break;
            }

            // Auto EPR: when the charger supports EPR and the current contract is an SPR fixed PDO, enter EPR mode (then request the same PDO, voltage unchanged)
            if (st.auto_epr && st.src_epr_capable && !st.epr_mode && !pe.epr_auto_failed && !pe.epr_failed &&
                st.explicit_contract && st.contract.kind == PD_PDO_FIXED && sink_tx_ok())
            {
                set_state(PE_SNK_Send_EPR_Mode_Entry);
                break;
            }
            // New target / retry after Wait
            if ((pe.target_dirty || (pe.retry_at && (int32_t)(pe.now - pe.retry_at) >= 0)) && sink_tx_ok())
            {
                pe.retry_at = 0;
                set_state(PE_SNK_Evaluate_Capability);
                break;
            }
            // SinkPPSPeriodicTimer: a PPS contract must be requested again periodically
            if (st.contract.kind == PD_PDO_SPR_PPS && (int32_t)(pe.now - pe.pps_at) >= 0 && sink_tx_ok())
            {
                pe.sel.pos = st.contract.pos;   // pe.sel is still the PDO of the current contract
                set_state(PE_SNK_Select_Capability);
                break;
            }
            // SinkEPRKeepAliveTimer
            if (st.epr_mode)
            {
                if (pe.keepalive_wait && (int32_t)(pe.now - pe.keepalive_deadline) >= 0)
                {
                    pe.keepalive_wait = false;
                    if (++pe.keepalive_miss >= 3)
                    {
                        enter_hard_reset(LINK_HR_KEEPALIVE);
                        break;
                    }
                }
                if (!pe.keepalive_wait && (int32_t)(pe.now - pe.keepalive_at) >= 0)
                {
                    pe.keepalive_at = pe.now + PD_tSinkEPRKeepAlive;
                    pe.keepalive_wait = true;
                    pe.keepalive_deadline = pe.now + PD_tSenderResponse * 3;
                    prl_tx_ext_ctrl(PD_EXT_CTRL_EPR_KeepAlive, 0);
                }
            }
            break;

        case PE_SNK_Send_EPR_Mode_Entry:
            if (pe.entry)
            {
                if (!sink_tx_ok()) break;
                pe.entry = false;
                uint32_t d = PD_EPR_MODE_DO(PD_EPR_MODE_Enter, PD_SINK_EPR_PDP_W);
                if (!prl_tx_data(PD_DATA_EPR_Mode, &d, 1)) set_state(PE_Send_Soft_Reset);
                break;
            }
            if (m && is_epr_mode(m, PD_EPR_MODE_Enter_Acknowledged))
            {
                set_state(PE_SNK_EPR_Mode_Entry_Wait_For_Response);
            }
            else if (m && is_epr_mode(m, PD_EPR_MODE_Enter_Failed))
            {
                epr_entry_failed(PD_EPR_MODE_DATA(m->obj[0]));
            }
            else if (m && (is_ctrl(m, PD_CTRL_Not_Supported) || is_ctrl(m, PD_CTRL_Reject)))
            {
                epr_entry_failed(0xFE);
            }
            else if (elapsed(PD_tSenderResponse))
            {
                set_state(PE_Send_Soft_Reset);
            }
            break;

        case PE_SNK_EPR_Mode_Entry_Wait_For_Response:
            if (m && is_epr_mode(m, PD_EPR_MODE_Enter_Succeeded))
            {
                st.epr_mode = true;
                pe.epr_entry_pending = false;
                pe.keepalive_at = pe.now + PD_tSinkEPRKeepAlive;
                pe.keepalive_miss = 0;
                st.epr_fail_reason = 0xFF;
                link_send_event(LINK_EVENT_EPR_ENTERED, 0);
                // The partner then sends EPR_Source_Capabilities
                set_state(PE_SNK_Wait_for_Capabilities);
                st.explicit_contract = true;
            }
            else if (m && is_epr_mode(m, PD_EPR_MODE_Enter_Failed))
            {
                epr_entry_failed(PD_EPR_MODE_DATA(m->obj[0]));
            }
            else if (elapsed(PD_tEnterEPR))
            {
                set_state(PE_Send_Soft_Reset);
            }
            break;

        case PE_SNK_Get_Source_Cap:
            if (pe.entry)
            {
                if (!sink_tx_ok()) break;
                pe.entry = false;
                pe.tries++;
                if (st.epr_mode) prl_tx_ext_ctrl(PD_EXT_CTRL_EPR_Get_Source_Cap, 0);
                else prl_tx_ctrl(PD_CTRL_Get_Source_Cap);
                break;
            }
            if (m && take_caps(m))
            {
                set_state(PE_SNK_Evaluate_Capability);
            }
            else if (elapsed(PD_tSenderResponse * 3))
            {
                if (pe.tries < CFG_RESUME_TRIES) set_state(PE_SNK_Get_Source_Cap);
                else
                {
                    if (pe.resume) link_send_event(LINK_EVENT_RESUME_FAILED, 0);
                    pe.resume = false;
                    set_state(PE_Send_Soft_Reset);
                }
            }
            break;

        case PE_SNK_EPR_Keepalive:
            // Keeping an EPR contract at boot: a successful keep-alive means still in EPR mode; then fetch the EPR source capabilities
            if (pe.entry)
            {
                pe.entry = false;
                pe.tries++;
                prl_tx_ext_ctrl(PD_EXT_CTRL_EPR_KeepAlive, 0);
                break;
            }
            if (m && is_ext_ctrl(m, PD_EXT_CTRL_EPR_KeepAlive_Ack))
            {
                st.epr_mode = true;
                pe.keepalive_at = pe.now + PD_tSinkEPRKeepAlive;
                pe.tries = 0;
                set_state(PE_SNK_Get_Source_Cap);
            }
            else if (m && take_caps(m))
            {
                set_state(PE_SNK_Evaluate_Capability);
            }
            else if (m && (is_ctrl(m, PD_CTRL_Not_Supported) || is_ctrl(m, PD_CTRL_Reject)))
            {
                // Not in EPR mode: fetch the SPR source capabilities instead
                pe.tries = 0;
                set_state(PE_SNK_Get_Source_Cap);
            }
            else if (elapsed(PD_tSenderResponse * 2))
            {
                if (pe.tries < CFG_RESUME_TRIES) set_state(PE_SNK_EPR_Keepalive);
                else
                {
                    pe.tries = 0;
                    set_state(PE_SNK_Get_Source_Cap);
                }
            }
            break;

        case PE_SNK_Send_EPR_Mode_Exit:
            if (pe.entry)
            {
                if (!sink_tx_ok()) break;
                pe.entry = false;
                uint32_t d = PD_EPR_MODE_DO(PD_EPR_MODE_Exit, 0);
                if (!prl_tx_data(PD_DATA_EPR_Mode, &d, 1)) set_state(PE_Send_Soft_Reset);
                break;
            }
            // The partner then sends SPR Source_Capabilities
            if (m && take_caps(m)) set_state(PE_SNK_Evaluate_Capability);
            else if (elapsed(PD_tSenderResponse * 3)) set_state(PE_Send_Soft_Reset);
            break;

        case PE_Send_Soft_Reset:
            if (pe.entry)
            {
                pe.entry = false;
                prl_reset();
                clear_contract();
                link_send_event(LINK_EVENT_SOFT_RESET, pe.sr_arg ? pe.sr_arg : 1);
                pe.sr_arg = 0;
                if (!prl_tx_ctrl(PD_CTRL_Soft_Reset)) enter_hard_reset(LINK_HR_SOFT_RESET_TX);
                break;
            }
            if (m && is_ctrl(m, PD_CTRL_Accept)) set_state(PE_SNK_Wait_for_Capabilities);
            else if (m && take_caps(m)) set_state(PE_SNK_Evaluate_Capability);
            else if (elapsed(PD_tSenderResponse)) enter_hard_reset(LINK_HR_SOFT_RESET_NO_RESP);
            break;

        case PE_SNK_Soft_Reset:
            clear_contract();
            link_send_event(LINK_EVENT_SOFT_RESET, 0);
            if (prl_tx_ctrl(PD_CTRL_Accept)) set_state(PE_SNK_Wait_for_Capabilities);
            else enter_hard_reset(LINK_HR_SOFT_RESET_ACK_TX);
            break;

        case PE_SNK_Hard_Reset:
            if (pe.hard_reset_count >= PD_nHardResetCount)
            {
                // The partner does not respond to PD: keep the implicit contract (vSafe5V); 2.2.3+ reports giving up (previously no event was left)
                link_send_event(LINK_EVENT_HARD_RESET_GIVEUP, LINK_HR_ARG(pe.hr_reason, pe.hr_waited));
                set_state(PE_SNK_Wait_for_Capabilities);
                break;
            }
            pe.hard_reset_count++;
            st.hard_resets++;
            prl_tx_hard_reset();
            link_send_event(LINK_EVENT_HARD_RESET_TX, LINK_HR_ARG(pe.hr_reason, pe.hr_waited));
            set_state(PE_SNK_Transition_to_default);
            break;

        case PE_SNK_Transition_to_default:
            if (pe.entry)
            {
                pe.entry = false;
                clear_contract();
                prl_reset();
                pe.resume = false;
                pe.epr_failed = false;
                pe.hr_until = pe.now + PD_tHardResetRecover;
                pe.hr_vbus_dropped = false;
                break;
            }
            // VBUS dropped near vSafe0V and then recovered to vSafe5V, or the recovery period ended
            if (vbus_mv() < 1000) pe.hr_vbus_dropped = true;
            if ((pe.hr_vbus_dropped && vbus_mv() >= CFG_VBUS_PRESENT_MV) || (int32_t)(pe.now - pe.hr_until) >= 0)
            {
                pe.hr_until = 0;
                set_state(PE_SNK_Startup);
            }
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------
void pe_init(void)
{
    memset(&st, 0, sizeof(st));
    memset(&pe, 0, sizeof(pe));
    st.state = PE_SNK_Detached;
    st.spec_rev = PD_REV_30;
    st.auto_epr = true;
    st.epr_fail_reason = 0xFF;
    const nv_target_t *t = nv_target();
    if (t)
    {
        st.target.mode = t->mode;
        st.target.flags = (uint8_t)(t->flags & ~PE_TARGET_PERSIST);
        st.target.mv = t->mv;
        st.target.ma = t->ma;
    }
}

void pe_attach(bool resume)
{
    pe.attached = true;
    pe.resume = resume;
    pe.hard_reset_count = 0;
    pe.soft_reset_tried = false;
    pe.no_accept_sr = false;
    pe.sr_arg = 0;
    pe.epr_failed = false;
    pe.epr_auto_failed = false;
    pe.caps_fresh = false;
    st.epr_fail_reason = 0xFF;
    pe.keepalive_at = 0;
    st.n_caps = 0;
    st.spec_rev = PD_REV_30;
    prl_set_spec_rev(PD_REV_30);
    pe.now = sys_time_ms();
    set_state(PE_SNK_Startup);
}

void pe_detach(void)
{
    pe.attached = false;
    pe.resume = false;
    clear_contract();
    st.n_caps = 0;
    st.caps_seq++;
    st.rp = PHY_RP_NONE;
    set_state(PE_SNK_Detached);
}

bool pe_in_hard_reset_recovery(void)
{
    return pe.attached && (st.state == PE_SNK_Transition_to_default || st.state == PE_SNK_Hard_Reset);
}

void pe_run(uint32_t now_ms)
{
    pe.now = now_ms;
    if (!pe.attached) return;

    if (phy_hard_reset_received())
    {
        st.hard_resets++;
        link_send_event(LINK_EVENT_HARD_RESET_RX, LINK_HR_ARG(st.state, now_ms - pe.t_entry));
        set_state(PE_SNK_Transition_to_default);
    }
    if (pe.hard_reset_req)
    {
        pe.hard_reset_req = false;
        pe.hard_reset_count = 0;
        enter_hard_reset(LINK_HR_HOST);
    }

    // Rp level (for reporting)
    if ((uint32_t)(now_ms - pe.rp_at) >= 200)
    {
        pe.rp_at = now_ms;
        phy_rp_t rp = phy_rp_level();
        if (rp != st.rp)
        {
            st.rp = rp;
            bump();
        }
    }

    prl_msg_t msg;
    const prl_msg_t *m = NULL;
    if (prl_rx(&msg, now_ms)) m = &msg;

    // Soft_Reset received in any state (except while handling Hard Reset)
    if (m && is_ctrl(m, PD_CTRL_Soft_Reset) && st.state != PE_SNK_Transition_to_default)
    {
        set_state(PE_SNK_Soft_Reset);
        m = NULL;
    }

    pe_state_t before = st.state;
    run_state(m);
    // After a state change, immediately run the new state's entry action (without waiting for the next round)
    for (uint8_t i = 0; i < 4 && st.state != before; i++)
    {
        before = st.state;
        run_state(NULL);
    }
}

link_result_t pe_set_target(const pe_target_t *t)
{
    if (t->mode > PD_SELECT_ADJ_FIRST || t->mv < 3300 || t->mv > 48000) return LINK_RESULT_BAD_ARG;

    link_result_t res = LINK_RESULT_OK;
    if (!pe.attached || !st.n_caps)
    {
        res = LINK_RESULT_NO_SOURCE;
    }
    else
    {
        // Pre-check: EPR target (not yet in EPR) or a usable PDO exists in the source capabilities
        bool epr_later = t->mv > 20000 && !st.epr_mode && st.src_epr_capable && !(t->flags & PE_TARGET_NO_EPR);
        pd_select_t s;
        if (!epr_later)
        {
            if (!pd_select(st.caps, st.n_caps, t->mode, t->mv, t->ma, &s)) return LINK_RESULT_NO_MATCH;
            if (s.clamped) res = LINK_RESULT_OK_CLAMPED;
        }
    }

    st.target = *t;
    st.target.flags &= (uint8_t)~PE_TARGET_PERSIST;
    pe.target_dirty = true;
    pe.epr_failed = false;
    bump();

    if (t->flags & PE_TARGET_PERSIST)
    {
        nv_target_t nv = {.mode = t->mode, .flags = (uint8_t)(t->flags & ~PE_TARGET_PERSIST), .mv = t->mv, .ma = t->ma};
        nv_save_target(&nv);
    }
    return res;
}

void pe_set_auto_epr(bool on)
{
    if (st.auto_epr == on) return;
    st.auto_epr = on;
    bump();
}

void pe_request_hard_reset(void)
{
    pe.hard_reset_req = true;
}

const pe_status_t *pe_status(void)
{
    return &st;
}
