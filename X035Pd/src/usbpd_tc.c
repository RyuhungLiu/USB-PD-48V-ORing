#include "usbpd_tc.h"

#include "config.h"
#include "link.h"
#include "usbpd_def.h"
#include "usbpd_pe.h"
#include "usbpd_phy.h"
#include "vbus.h"

static tc_state_t state;
static uint8_t    cc;
static uint32_t   since;            // time of entering AttachWait / VBUS dropping below threshold
static bool       first_attach = true;
static uint16_t   er_request_ms;    // pending ErrorRecovery, 0 = none
static uint16_t   er_ms;

void tc_init(void)
{
    state = TC_Unattached_SNK;
    cc = 0;
}

static void attach(void)
{
    // First attach after boot with VBUS already at an explicit contract voltage: keep the contract (no waiting for source capabilities, no Hard Reset)
    bool resume = first_attach && vbus_mv() >= CFG_RESUME_VBUS_MV;
    first_attach = false;
    state = TC_Attached_SNK;
    since = 0;
    phy_attach(cc);
    pe_attach(resume);
    link_send_event(LINK_EVENT_ATTACH, cc);
}

void tc_request_error_recovery(uint16_t ms)
{
    er_request_ms = (ms < PD_tErrorRecovery_min) ? PD_tErrorRecovery_min : ms;
}

void tc_run(uint32_t now_ms)
{
    if (er_request_ms && state != TC_ErrorRecovery)
    {
        if (state == TC_Attached_SNK)
        {
            phy_detach();
            pe_detach();
            link_send_event(LINK_EVENT_DETACH, 0);
        }
        phy_cc_open();
        first_attach = false;           // after recovery, treat as a new attach (do not keep the contract)
        er_ms = er_request_ms;
        er_request_ms = 0;
        cc = 0;
        since = now_ms;
        state = TC_ErrorRecovery;
        link_send_event(LINK_EVENT_ERROR_RECOVERY, er_ms);
        return;
    }

    switch (state)
    {
        case TC_Unattached_SNK:
        {
            uint8_t det = phy_cc_detect();
            if (!det) break;
            cc = (det & 1) ? 1 : 2;     // when both lines have Rp (some A-to-C cables), take CC1
            state = TC_AttachWait_SNK;
            since = now_ms;
            break;
        }

        case TC_AttachWait_SNK:
        {
            uint8_t det = phy_cc_detect();
            if (!(det & (1u << (cc - 1))))
            {
                state = TC_Unattached_SNK;
                break;
            }
            // VBUS already at high voltage at boot (keeping the contract): skip debounce, take over EPR keep-alive as soon as possible
            bool hurry = first_attach && vbus_mv() >= CFG_RESUME_VBUS_MV;
            if ((hurry || (uint32_t)(now_ms - since) >= PD_tCCDebounce) && vbus_mv() >= CFG_VBUS_PRESENT_MV)
                attach();
            break;
        }

        case TC_Attached_SNK:
            if (vbus_mv() >= CFG_VBUS_DISCONNECT_MV || pe_in_hard_reset_recovery())
            {
                since = 0;
                break;
            }
            if (!since) since = now_ms ? now_ms : 1;
            if ((uint32_t)(now_ms - since) < CFG_DETACH_DEBOUNCE_MS) break;
            phy_detach();
            pe_detach();
            link_send_event(LINK_EVENT_DETACH, 0);
            state = TC_Unattached_SNK;
            cc = 0;
            break;

        case TC_ErrorRecovery:
            if ((uint32_t)(now_ms - since) < er_ms) break;
            phy_cc_release();
            state = TC_Unattached_SNK;
            break;
    }
}

tc_state_t tc_state(void)
{
    return state;
}

uint8_t tc_attached_cc(void)
{
    return state == TC_Attached_SNK ? cc : 0;
}
