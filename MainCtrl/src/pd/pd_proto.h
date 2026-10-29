#pragma once

#include <stdbool.h>
#include <stdint.h>

// UART protocol with the X035 (USB_PD_Trigger APP), defined in its APP/src/config.h
//
// Device -> main controller (reports)
//   Mini: [A2 FF][Len][Type][Data...]                      total length = 3  + Len
//   Std : [A1 FF][Time 4B][Counter 4B][Len][Type][Data...]  total length = 11 + Len
//   Len = number of bytes of Type + Data
//
// Main controller -> device (commands, Mini only)
//   [52 FF][Len][Type=01][Category][Cmd][Value int32 LE] + 1 byte padding
//   Len = 7. The X035 USART packet assembler receives 4+Len bytes (1 byte more than the actual content),
//   so 0x00 is appended at the end; if the X035 is later fixed to 3+Len, the extra byte is discarded by header sync, so it stays compatible.

#define PD_PROTO_MAX_DATA       52      // LOG_DATA_MAX_LEN
#define PD_PROTO_CMD_LEN        11

// Report Type
#define PD_REPORT_USB_PD        0x00
#define PD_REPORT_SYSTEM        0x01
#define PD_REPORT_MESSAGE       0x02

// Command Category
#define PD_CMD_CAT_PD           0x00
#define PD_CMD_CAT_SYS          0x01

// SYS command codes (excerpt)
#define PD_SYS_USB_PD_REBOOT    0x01    // send Hard Reset to renegotiate
#define PD_SYS_GET_STATUS       0x02
#define PD_SYS_SINK_SPR         0x31
#define PD_SYS_SINK_EPR         0x32
#define PD_SYS_REPORT_STD       0x41
#define PD_SYS_REPORT_MINI      0x42
#define PD_SYS_TRIGGER_HOLD_PRESET 0x64
#define PD_SYS_PRESET_VOLTAGE   0x65
#define PD_SYS_PRESET_CURRENT   0x66
#define PD_SYS_TRIGGER_HOLD_AP  0x67    // match the preset voltage; with current -1 take the PDO maximum current
#define PD_SYS_FLASH_FB_ON      0x82    // enable flash fallback storage (trigger hold depends on the NV backend)
#define PD_SYS_RDO_PERSIST_OFF  0xA1    // [local branch 1.2.90+] runtime PD requests do not write NV (RAM flag, writing resumes after X035 reset)
#define PD_SYS_RDO_PERSIST_ON   0xA2

// PD command codes (Category = PD_CMD_CAT_PD)
#define PD_PD_SET_VOLTAGE_MV    0x03    // valid only for PPS/AVS PDOs
#define PD_PD_INCREASE_MV       0x06
#define PD_PD_DECREASE_MV       0x07
#define PD_PD_REQUEST_VOLT_CURR 0x0C    // value = mV | (mA << 16), prefers an exactly matching fixed PDO
#define PD_PD_REQUEST_VC_ADJ    0x0D    // [local branch] same as above, prefers adjustable PPS/AVS PDOs

// PD command acknowledgement result ([local branch] ack [0x00, code, value(4), result])
#define PD_RESULT_OK            0
#define PD_RESULT_OK_CLAMPED    1
#define PD_RESULT_NO_SOURCE     2
#define PD_RESULT_WRONG_PDO     3
#define PD_RESULT_NO_MATCH      4
#define PD_RESULT_QUEUE_FULL    5
#define PD_RESULT_NONE          0xFF    // no acknowledgement received yet

// Message report tag data[0] (0x00/0x01 are command acks, 0x02 is IO ack, 0x01 with 10 bytes is ADC log)
#define PD_MSG_TAG_CONTRACT     0x04    // [local branch] contract report, pushed on PS_RDY
#define PD_MSG_TAG_BOOT         0x05    // [local branch] X035 boot report

// SOP code in data[0] of USB PD frames within a report
#define PD_SOP_HARD_RESET       5
#define PD_SYS_ADC_LOG_OFF      0x71
#define PD_SYS_ADC_LOG_ON       0x72
#define PD_SYS_ADC_LOG_FREQ     0x73

// System event codes
#define PD_EVT_ATTACH_CC1       0x11
#define PD_EVT_DETACH_CC1       0x12
#define PD_EVT_ATTACH_CC2       0x21
#define PD_EVT_DETACH_CC2       0x22
#define PD_EVT_VBUS_ON          0x31
#define PD_EVT_VBUS_OFF         0x32
#define PD_EVT_CRC_ERROR        0xEE

typedef struct pd_frame_t
{
    uint8_t type;
    uint8_t len;                        // number of data bytes
    uint8_t data[PD_PROTO_MAX_DATA];
} pd_frame_t;

typedef struct pd_parser_t
{
    uint8_t  buf[64];
    uint8_t  pos;
    uint8_t  expected;                  // 0 = length unknown
    uint32_t last_byte_ms;
    uint32_t bad_frames;
} pd_parser_t;

void pd_parser_reset(pd_parser_t *p);

// Feed byte by byte; fills out and returns true when a complete report is received
bool pd_parser_feed(pd_parser_t *p, uint8_t byte, uint32_t now_ms, pd_frame_t *out);

// Build a SYS/PD command, returns its length (PD_PROTO_CMD_LEN)
uint8_t pd_build_cmd(uint8_t *out, uint8_t category, uint8_t cmd, int32_t value);
