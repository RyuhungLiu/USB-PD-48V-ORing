#pragma once

// USB Power Delivery R3.2 definitions: message types, header fields, data objects, timing constants
// Names match the spec text (Message Type names, Timer / Counter names)

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// 6.2.1.1 Message Header
// ---------------------------------------------------------------------------
#define PD_HEADER_TYPE(h)           ((uint8_t)((h) & 0x1F))
#define PD_HEADER_DATA_ROLE(h)      (((h) >> 5) & 0x1)
#define PD_HEADER_SPEC_REV(h)       (((h) >> 6) & 0x3)
#define PD_HEADER_POWER_ROLE(h)     (((h) >> 8) & 0x1)
#define PD_HEADER_MESSAGE_ID(h)     ((uint8_t)(((h) >> 9) & 0x7))
#define PD_HEADER_NUM_DO(h)         ((uint8_t)(((h) >> 12) & 0x7))
#define PD_HEADER_EXTENDED(h)       (((h) >> 15) & 0x1)

#define PD_REV_20                   1
#define PD_REV_30                   2       // Revision 3.x

// 6.2.1.2 Extended Message Header
#define PD_EXT_HEADER_DATA_SIZE(e)  ((uint16_t)((e) & 0x1FF))
#define PD_EXT_HEADER_REQUEST(e)    (((e) >> 10) & 0x1)
#define PD_EXT_HEADER_CHUNK_NUM(e)  ((uint8_t)(((e) >> 11) & 0xF))
#define PD_EXT_HEADER_CHUNKED(e)    (((e) >> 15) & 0x1)
#define PD_EXT_HEADER(size, req, num, chunked) \
    ((uint16_t)(((size) & 0x1FF) | ((req) << 10) | (((num) & 0xF) << 11) | ((chunked) << 15)))

#define PD_MAX_DATA_OBJECTS         7
#define PD_MAX_EXT_CHUNK_BYTES      26
#define PD_MAX_EXT_DATA_BYTES       260

// ---------------------------------------------------------------------------
// 6.3 Control Message types
// ---------------------------------------------------------------------------
typedef enum pd_ctrl_msg_t
{
    PD_CTRL_GoodCRC                 = 0x01,
    PD_CTRL_GotoMin                 = 0x02,
    PD_CTRL_Accept                  = 0x03,
    PD_CTRL_Reject                  = 0x04,
    PD_CTRL_Ping                    = 0x05,
    PD_CTRL_PS_RDY                  = 0x06,
    PD_CTRL_Get_Source_Cap          = 0x07,
    PD_CTRL_Get_Sink_Cap            = 0x08,
    PD_CTRL_DR_Swap                 = 0x09,
    PD_CTRL_PR_Swap                 = 0x0A,
    PD_CTRL_VCONN_Swap              = 0x0B,
    PD_CTRL_Wait                    = 0x0C,
    PD_CTRL_Soft_Reset              = 0x0D,
    PD_CTRL_Data_Reset              = 0x0E,
    PD_CTRL_Data_Reset_Complete     = 0x0F,
    PD_CTRL_Not_Supported           = 0x10,
    PD_CTRL_Get_Source_Cap_Extended = 0x11,
    PD_CTRL_Get_Status              = 0x12,
    PD_CTRL_FR_Swap                 = 0x13,
    PD_CTRL_Get_PPS_Status          = 0x14,
    PD_CTRL_Get_Country_Codes       = 0x15,
    PD_CTRL_Get_Sink_Cap_Extended   = 0x16,
    PD_CTRL_Get_Source_Info         = 0x17,
    PD_CTRL_Get_Revision            = 0x18,
} pd_ctrl_msg_t;

// 6.4 Data Message types
typedef enum pd_data_msg_t
{
    PD_DATA_Source_Capabilities     = 0x01,
    PD_DATA_Request                 = 0x02,
    PD_DATA_BIST                    = 0x03,
    PD_DATA_Sink_Capabilities       = 0x04,
    PD_DATA_Battery_Status          = 0x05,
    PD_DATA_Alert                   = 0x06,
    PD_DATA_Get_Country_Info        = 0x07,
    PD_DATA_Enter_USB               = 0x08,
    PD_DATA_EPR_Request             = 0x09,
    PD_DATA_EPR_Mode                = 0x0A,
    PD_DATA_Source_Info             = 0x0B,
    PD_DATA_Revision                = 0x0C,
    PD_DATA_Vendor_Defined          = 0x0F,
} pd_data_msg_t;

// 6.5 Extended Message types
typedef enum pd_ext_msg_t
{
    PD_EXT_Source_Capabilities_Extended = 0x01,
    PD_EXT_Status                       = 0x02,
    PD_EXT_Get_Battery_Cap              = 0x03,
    PD_EXT_Get_Battery_Status           = 0x04,
    PD_EXT_Battery_Capabilities         = 0x05,
    PD_EXT_Get_Manufacturer_Info        = 0x06,
    PD_EXT_Manufacturer_Info            = 0x07,
    PD_EXT_Security_Request             = 0x08,
    PD_EXT_Security_Response            = 0x09,
    PD_EXT_Firmware_Update_Request      = 0x0A,
    PD_EXT_Firmware_Update_Response     = 0x0B,
    PD_EXT_PPS_Status                   = 0x0C,
    PD_EXT_Country_Info                 = 0x0D,
    PD_EXT_Country_Codes                = 0x0E,
    PD_EXT_Sink_Capabilities_Extended   = 0x0F,
    PD_EXT_Extended_Control             = 0x10,
    PD_EXT_EPR_Source_Capabilities      = 0x11,
    PD_EXT_EPR_Sink_Capabilities        = 0x12,
    PD_EXT_Vendor_Defined_Extended      = 0x1E,
} pd_ext_msg_t;

// 6.5.14 Extended_Control Message types
typedef enum pd_ext_ctrl_t
{
    PD_EXT_CTRL_EPR_Get_Source_Cap  = 0x01,
    PD_EXT_CTRL_EPR_Get_Sink_Cap    = 0x02,
    PD_EXT_CTRL_EPR_KeepAlive       = 0x03,
    PD_EXT_CTRL_EPR_KeepAlive_Ack   = 0x04,
} pd_ext_ctrl_t;

// 6.4.10 EPR_Mode Data Object: Action
typedef enum pd_epr_mode_action_t
{
    PD_EPR_MODE_Enter               = 0x01,
    PD_EPR_MODE_Enter_Acknowledged  = 0x02,
    PD_EPR_MODE_Enter_Succeeded     = 0x03,
    PD_EPR_MODE_Enter_Failed        = 0x04,
    PD_EPR_MODE_Exit                = 0x05,
} pd_epr_mode_action_t;

#define PD_EPR_MODE_DO(action, data)    (((uint32_t)(action) << 24) | ((uint32_t)(data) << 16))
#define PD_EPR_MODE_ACTION(d)           ((uint8_t)((d) >> 24))
#define PD_EPR_MODE_DATA(d)             ((uint8_t)((d) >> 16))

// ---------------------------------------------------------------------------
// 6.4.1 Power Data Objects (kind numbers match the main controller V203 pdo_kind_t)
// ---------------------------------------------------------------------------
typedef enum pd_pdo_kind_t
{
    PD_PDO_NONE = 0,
    PD_PDO_FIXED,
    PD_PDO_BATTERY,
    PD_PDO_VARIABLE,
    PD_PDO_SPR_PPS,
    PD_PDO_EPR_AVS,
    PD_PDO_SPR_AVS,
} pd_pdo_kind_t;

#define PD_PDO_FIXED_EPR_MODE_CAPABLE   (1u << 23)  // Source PDO1: EPR mode supported
#define PD_PDO_FIXED_UNCONSTRAINED      (1u << 27)

// 6.4.2 Request Data Object common fields
#define PD_RDO_OBJ_POS(pos)             ((uint32_t)((pos) & 0xF) << 28)
#define PD_RDO_CAP_MISMATCH             (1u << 26)
#define PD_RDO_NO_USB_SUSPEND           (1u << 24)
#define PD_RDO_EPR_MODE_CAPABLE         (1u << 22)

// ---------------------------------------------------------------------------
// 6.6 Timers (unit ms; values taken within the spec range)
// ---------------------------------------------------------------------------
#define PD_tTypeCSinkWaitCap            465     // 310 .. 620
#define PD_tSenderResponse              30      // 27 .. 33
#define PD_tPSTransition_SPR            550     // 450 .. 550
#define PD_tPSTransition_EPR            1020    // 830 .. 1020
#define PD_tSinkRequest                 100
#define PD_tPPSRequest                  5000    // spec maximum 10s
#define PD_tSinkEPRKeepAlive            300     // 250 .. 500
#define PD_tEnterEPR                    550     // 450 .. 550
#define PD_tChunkSenderRequest          27      // 24 .. 30
#define PD_tHardResetRecover            3000    // upper limit for waiting for VBUS to recover after Hard Reset (tSafe0V + tSrcRecover + tSrcTurnOn plus margin)
#define PD_tReceive_us                  1000    // 0.9 .. 1.1 ms
#define PD_tCCDebounce                  120     // 100 .. 200
#define PD_tErrorRecovery_min           25      // Type-C: minimum time terminations are removed for ErrorRecovery

// 6.7 Counters
#define PD_nRetryCount                  2
#define PD_nHardResetCount              2
#define PD_nCapsCount                   50

// EPR operational power of this Sink (Data field of EPR_Mode Enter, W)
#define PD_SINK_EPR_PDP_W               240
