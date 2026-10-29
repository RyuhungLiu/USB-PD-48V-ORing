#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "board.h"
#include "pd/pd_decode.h"

// Link and status mirror of each X035 (X035Pd firmware 2.x, protocol see X035Pd/docs/PROTOCOL.md)
//   The X035 only does PD sink negotiation and reporting: it reports STATUS on its own when the state changes, otherwise once every 500ms (doubles as heartbeat)

#define PD_MAX_CAPS 11      // SPR has 7 PDOs; the EPR capability table has at most 11 (1..7 are the SPR part)

// SET_TARGET PDO selection mode (code of pd_node_send_pd_cmd)
#define PD_TARGET_FIXED_FIRST   0       // fixed PDO with matching voltage first, then PPS -> SPR AVS -> EPR AVS
#define PD_TARGET_ADJ_FIRST     1       // PPS -> SPR AVS -> EPR AVS first, then fixed PDO

// SET_TARGET acknowledgement result (last_cmd_result)
#define PD_RESULT_OK            0
#define PD_RESULT_OK_CLAMPED    1
#define PD_RESULT_NO_SOURCE     2       // not connected or no capability table yet (target kept, applied once connected)
#define PD_RESULT_NO_MATCH      3       // no usable PDO, current contract kept
#define PD_RESULT_BAD_ARG       4
#define PD_RESULT_NONE          0xFF    // no acknowledgement received yet

// Voltage switching progress
typedef enum pd_vset_t
{
    PD_VSET_IDLE = 0,
    PD_VSET_BUSY,           // command sent, waiting for the new contract
    PD_VSET_OK,             // new contract voltage = target
    PD_VSET_FALLBACK,       // timed out without reaching the target
    PD_VSET_STORED,         // no charger connected: target written to X035, negotiated per target when plugged in
} pd_vset_t;

typedef enum pd_link_t
{
    PD_LINK_OFFLINE = 0,    // no valid report received
    PD_LINK_ONLINE,
} pd_link_t;

typedef enum pd_cc_t
{
    PD_CC_NONE = 0,
    PD_CC_1,
    PD_CC_2,
} pd_cc_t;

typedef struct pd_node_t
{
    pd_link_t link;
    uint32_t  last_rx_ms;

    // INFO
    bool      version_valid;
    uint8_t   fw_major, fw_minor, fw_patch;
    uint8_t   x035_reset_flags;     // RSTSCKR >> 24
    uint32_t  x035_boots;           // number of INFO messages received (unsolicited report at boot)

    // STATUS
    bool      status_valid;
    bool      config_aligned;       // the new protocol has no config to align: true once STATUS is received
    uint8_t   pe_state;
    uint8_t   rp;                   // 0 none / 1 Default / 2 1.5A / 3 3.0A
    bool      src_epr_capable;
    bool      busy;                 // X035 negotiation in progress
    bool      target_unmet;
    uint8_t   last_result;          // result of the latest negotiation
    uint16_t  preset_mv;            // current X035 target voltage (boot / PERSIST target)
    uint8_t   target_mode;
    uint16_t  crc_errors;
    uint16_t  tx_fails;
    uint8_t   epr_fail_reason;      // X035 2.1+: reason EPR entry failed (0xFF none; 0xFE rejected; 1 cable ...)
    bool      auto_epr;             // X035 2.1+: whether Auto EPR is currently on
    bool      has_auto_epr;         // X035 supports Auto EPR (STATUS >= 26 bytes)

    // PD contract
    uint8_t    n_caps;
    pdo_info_t caps[PD_MAX_CAPS];
    uint8_t    caps_seq;            // sequence of the capability table received
    uint8_t    caps_seq_pending;    // capability table sequence reported by STATUS
    bool       contract_valid;
    pdo_kind_t contract_kind;
    uint16_t   contract_mv;
    uint16_t   contract_ma;
    bool       epr;                 // in EPR mode
    uint8_t    contract_pos;        // contract PDO position (1-based)
    uint32_t   contract_seq;        // +1 on every new contract (including PS_RDY of periodic PPS requests)

    // Voltage switching
    pd_vset_t vset;
    uint16_t  vset_target_mv;
    uint32_t  vset_start_ms;

    // Events
    pd_cc_t   cc;
    uint32_t  detach_count;         // +1 on every Detach, used by the ORing interlock to detect unplugging
    uint32_t  hard_reset_count;     // +1 on every Hard Reset (received / sent), used by the ORing interlock

    // VBUS measured by the X035 ADC
    bool      vbus_valid;
    uint16_t  vbus_mv;
    uint32_t  vbus_ms;

    // Legacy field: always true for X035Pd 2.x (runtime requests do not write X035 flash)
    bool      ext_proto;
    bool      rdo_volatile;

    // Latest command acknowledgement
    uint8_t   last_cmd_code;
    uint8_t   last_cmd_result;      // PD_RESULT_*

    // Statistics
    uint32_t  rx_frames;
    uint32_t  bad_frames;

    // Latest Hard Reset event (X035 EVENT arrives before the STATUS count): code = PD_EVT_HARD_RESET_RX / _TX,
    // arg (X035 2.2.1+) upper 5 bits = PE state on RX / initiating reason on TX, lower 11 bits = ms already spent in that state
    uint8_t   last_hr_code;
    uint16_t  last_hr_arg;

    // Contract just became invalid (X035 does not report a contract during Soft_Reset, VBUS unchanged): remember the contract current and time before it was lost; OCP keeps using it briefly
    uint16_t  held_ma;
    uint32_t  lost_ms;
} pd_node_t;

// X035 EVENT codes (X035Pd docs/PROTOCOL.md)
#define PD_EVT_HARD_RESET_RX    3
#define PD_EVT_HARD_RESET_TX    4

void pd_node_init(void);

// Called every main loop iteration: receive packets, timeout judgment
void pd_node_process(uint32_t now_ms);

const pd_node_t *pd_node_get(ch_id_t ch);

// VBUS voltage is valid only if updated within PD_VBUS_STALE_MS
bool pd_node_vbus_fresh(ch_id_t ch, uint32_t now_ms);

const char *pd_node_link_str(pd_link_t link);

// Switch to the target voltage (fixed PDO first, written to X035 as boot target); above 20V the X035 enters EPR automatically.
// No more Hard Reset: VBUS is not interrupted during contract switching. The caller is still responsible for turning off that channel's MOS first. Returns false if the link is offline
bool pd_node_set_voltage(ch_id_t ch, uint16_t mv);

const char *pd_vset_str(pd_vset_t vset);

// Link online and STATUS received; commands can be sent
bool pd_node_ready(ch_id_t ch);

// Runtime request (does not write X035 flash): code = PD_TARGET_*, value = mV | (mA << 16) (mA = 0 takes the PDO's maximum)
// Acknowledgement see last_cmd_result. Returns false if the link is not ready
bool pd_node_send_pd_cmd(ch_id_t ch, uint8_t code, int32_t value);

const char *pd_result_str(uint8_t result);

// Whether the current X035 target is exactly mv (the main controller uses this to decide whether the boot target needs pushing)
bool pd_node_preset_matches(ch_id_t ch, uint16_t mv);

// Ask the X035 to send a Hard Reset
bool pd_node_hard_reset(ch_id_t ch);

// Ask the X035 (2.2+) to enter Type-C ErrorRecovery: remove the CC termination for ms milliseconds, the source sees an unplug and reconnects (software replug)
bool pd_node_error_recovery(ch_id_t ch, uint16_t ms);
