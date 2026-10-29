#pragma once

// USB Type-C Sink connection state machine (Type-C R2.3 4.5.2.2):
//   Unattached.SNK -> AttachWait.SNK (Rp present on CC, debounced for tCCDebounce) -> Attached.SNK (VBUS present)
//   Attached.SNK -> Unattached.SNK: VBUS below vSinkDisconnect (except during Hard Reset recovery)
//   Any state -> ErrorRecovery (requested by V203): terminations removed for tErrorRecovery; the source sees a detach, removes VBUS and resets the port,
//   then returns to Unattached.SNK and reconnects (equivalent to a software replug)

#include <stdbool.h>
#include <stdint.h>

typedef enum tc_state_t
{
    TC_Unattached_SNK = 0,
    TC_AttachWait_SNK,
    TC_Attached_SNK,
    TC_ErrorRecovery,
} tc_state_t;

void tc_init(void);

// Called every CFG_TC_PERIOD_MS
void tc_run(uint32_t now_ms);

tc_state_t tc_state(void);

// Request entering ErrorRecovery with terminations removed for ms milliseconds (no less than PD_tErrorRecovery_min); executed on the next tc_run
void tc_request_error_recovery(uint16_t ms);
uint8_t tc_attached_cc(void);       // 0 = not attached
