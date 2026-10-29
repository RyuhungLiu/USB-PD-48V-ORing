#pragma once

// UART protocol with the main controller V203 (docs/PROTOCOL.md)

#include <stdbool.h>
#include <stdint.h>

#define LINK_MAX_PAYLOAD        60

// V203 -> X035
#define LINK_REQ_GET_STATUS         0x01
#define LINK_REQ_SET_TARGET         0x02
#define LINK_REQ_GET_SOURCE_CAPS    0x03
#define LINK_REQ_HARD_RESET         0x04
#define LINK_REQ_CONFIG             0x05
#define LINK_REQ_REBOOT             0x06
#define LINK_REQ_GET_INFO           0x07
#define LINK_REQ_ERROR_RECOVERY     0x08        // 2.2+: Type-C ErrorRecovery (software replug)

// X035 -> V203
#define LINK_MSG_INFO               0x81
#define LINK_MSG_STATUS             0x82
#define LINK_MSG_SOURCE_CAPS        0x83
#define LINK_MSG_EVENT              0x84
#define LINK_MSG_RESULT             0x85
#define LINK_MSG_PD_MSG             0x86

typedef enum link_result_t
{
    LINK_RESULT_OK = 0,
    LINK_RESULT_OK_CLAMPED,
    LINK_RESULT_NO_SOURCE,
    LINK_RESULT_NO_MATCH,
    LINK_RESULT_BAD_ARG,
} link_result_t;

typedef enum link_event_t
{
    LINK_EVENT_ATTACH = 1,
    LINK_EVENT_DETACH,
    LINK_EVENT_HARD_RESET_RX,
    LINK_EVENT_HARD_RESET_TX,
    LINK_EVENT_SOFT_RESET,
    LINK_EVENT_EPR_ENTERED,
    LINK_EVENT_EPR_ENTER_FAILED,
    LINK_EVENT_REJECT,
    LINK_EVENT_WAIT,
    LINK_EVENT_RESUME_OK,
    LINK_EVENT_RESUME_FAILED,
    LINK_EVENT_CONTRACT,
    LINK_EVENT_ERROR_RECOVERY,                  // 2.2+: arg = milliseconds the terminations are removed
    LINK_EVENT_ACCEPT_LATE,                     // 2.2.2+: arg = milliseconds from request to Accept received (reported only if over tSenderResponse)
    LINK_EVENT_HARD_RESET_GIVEUP,               // 2.2.3+: HardResetCounter exhausted, no more Hard Reset sent; arg same as HARD_RESET_TX
} link_event_t;

// arg of HARD_RESET_RX / HARD_RESET_TX (2.2.1+): high 5 bits are the code, low 11 bits are milliseconds already spent in that state (max 2047)
//   RX: code = PE state when received (pe_state_t)
//   TX: code = reason (link_hr_reason_t)
#define LINK_HR_ARG(code, ms)   ((uint16_t)(((uint16_t)(code) << 11) | ((ms) > 2047 ? 2047 : (ms))))

typedef enum link_hr_reason_t
{
    LINK_HR_WAIT_CAP = 1,           // SinkWaitCapTimer timeout (Soft_Reset already tried or VBUS insufficient)
    LINK_HR_NO_ACCEPT,              // Select_Capability: no Accept / Reject within SenderResponseTimer
    LINK_HR_NO_PS_RDY,              // Transition_Sink: no PS_RDY within PSTransitionTimer
    LINK_HR_KEEPALIVE,              // EPR_KeepAlive: no Ack 3 times in a row
    LINK_HR_SOFT_RESET_TX,          // Send_Soft_Reset: failed to send Soft_Reset (no GoodCRC)
    LINK_HR_SOFT_RESET_NO_RESP,     // Send_Soft_Reset: no Accept received
    LINK_HR_SOFT_RESET_ACK_TX,      // Soft_Reset received, failed to send the Accept reply
    LINK_HR_HOST,                   // requested by V203 (HARD_RESET 0x04)
} link_hr_reason_t;

void link_init(void);

// Called by the main loop: parse received requests and execute them
void link_process(uint32_t now_ms);

void link_send(uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len);
void link_send_event(link_event_t code, uint16_t arg);

// PD message forwarding (when CONFIG bit0 is enabled)
void link_forward_pd(uint8_t dir, const uint8_t *buf, uint8_t len);
