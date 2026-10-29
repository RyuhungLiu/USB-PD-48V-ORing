#pragma once

// USB PD policy engine (spec 8.3.3.3 Sink Port State Diagram, including EPR)
// State names match the spec. Deviations from the spec:
//   - Keep contract at boot: when VBUS is already above vSafe5V, do not wait for Source_Capabilities; first use Get_Source_Cap
//     (in EPR mode, EPR_KeepAlive + EPR_Get_Source_Cap) to fetch the source capabilities and request again, avoiding interrupting VBUS where possible
//   - On SinkWaitCapTimer timeout, send Soft_Reset first; Hard Reset only if that fails

#include <stdbool.h>
#include <stdint.h>
#include "link.h"
#include "usbpd_phy.h"
#include "usbpd_pdo.h"

#define PE_MAX_CAPS     11

typedef enum pe_state_t
{
    PE_SNK_Startup = 0,
    PE_SNK_Discovery,
    PE_SNK_Wait_for_Capabilities,
    PE_SNK_Evaluate_Capability,
    PE_SNK_Select_Capability,
    PE_SNK_Transition_Sink,
    PE_SNK_Ready,
    PE_SNK_Hard_Reset,
    PE_SNK_Transition_to_default,
    PE_SNK_Get_Source_Cap,
    PE_SNK_Soft_Reset,
    PE_Send_Soft_Reset,
    PE_SNK_Send_EPR_Mode_Entry,
    PE_SNK_EPR_Mode_Entry_Wait_For_Response,
    PE_SNK_EPR_Keepalive,
    PE_SNK_Send_EPR_Mode_Exit,
    PE_SNK_Detached,                // not attached (non-spec state, for reporting)
} pe_state_t;

typedef struct pe_target_t
{
    uint8_t  mode;                  // PD_SELECT_FIXED_FIRST / PD_SELECT_ADJ_FIRST
    uint8_t  flags;                 // PE_TARGET_PERSIST / PE_TARGET_NO_EPR
    uint16_t mv;
    uint16_t ma;                    // 0 = maximum of that PDO
} pe_target_t;

#define PE_TARGET_PERSIST   0x01
#define PE_TARGET_NO_EPR    0x02

typedef enum pe_result_t
{
    PE_RESULT_OK = 0,
    PE_RESULT_NO_MATCH = 3,
    PE_RESULT_REJECT = 6,
    PE_RESULT_WAIT = 7,
    PE_RESULT_EPR_FAILED = 8,
    PE_RESULT_TIMEOUT = 9,
} pe_result_t;

typedef struct pe_contract_t
{
    uint8_t       pos;
    pd_pdo_kind_t kind;
    uint16_t      mv;
    uint16_t      ma;
} pe_contract_t;

typedef struct pe_status_t
{
    uint32_t      rev;              // incremented whenever any field changes (link reports immediately on this)
    pe_state_t    state;
    bool          explicit_contract;
    bool          epr_mode;
    bool          src_epr_capable;
    bool          busy;
    bool          target_unmet;
    uint8_t       spec_rev;
    phy_rp_t      rp;
    pe_contract_t contract;
    uint16_t      contract_seq;
    uint8_t       hard_resets;
    uint8_t       caps_seq;
    uint8_t       last_result;
    pe_target_t   target;
    uint8_t       n_caps;
    bool          caps_epr;
    bool          auto_epr;         // automatically enter EPR mode (voltage unchanged) when the charger supports EPR, to obtain the EPR source capabilities
    uint8_t       epr_fail_reason;  // reason of the most recent EPR entry failure: 0xFF none; 0xFE rejected / not supported; otherwise EPR_Mode Data
    uint32_t      caps[PE_MAX_CAPS];
} pe_status_t;

void pe_init(void);

// Called by the Type-C layer: attach / detach; resume = VBUS already at an explicit contract voltage at boot
void pe_attach(bool resume);
void pe_detach(void);

void pe_run(uint32_t now_ms);

// After Hard Reset VBUS drops to 0V and then recovers: during this period the Type-C layer does not detect detach
bool pe_in_hard_reset_recovery(void);

link_result_t pe_set_target(const pe_target_t *t);
void pe_request_hard_reset(void);
void pe_set_auto_epr(bool on);

const pe_status_t *pe_status(void);
