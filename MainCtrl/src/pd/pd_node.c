#include "pd_node.h"

#include <string.h>
#include "drivers/uart_link.h"
#include "bsp/delay.h"
#include "bsp/log.h"
#include "app/settings.h"

// Protocol (X035Pd/docs/PROTOCOL.md): [A5][LEN][SEQ][TYPE][PAYLOAD][CRC8]
#define FRAME_SOF               0xA5
#define FRAME_MAX_PAYLOAD       60

#define REQ_GET_STATUS          0x01
#define REQ_SET_TARGET          0x02
#define REQ_GET_SOURCE_CAPS     0x03
#define REQ_HARD_RESET          0x04
#define REQ_CONFIG              0x05
#define REQ_GET_INFO            0x07
#define REQ_ERROR_RECOVERY      0x08

#define MSG_INFO                0x81
#define MSG_STATUS              0x82
#define MSG_SOURCE_CAPS         0x83
#define MSG_EVENT               0x84
#define MSG_RESULT              0x85

#define TARGET_FLAG_PERSIST     0x01

#define STATUS_LEN              24
#define INFO_LEN                10

// Link judgment: the X035 reports STATUS on its own every 500ms
#define LINK_TIMEOUT_MS         2000
#define QUERY_RETRY_MS          1000    // re-query period while the version / capability table is unknown

// Timeout waiting for the new contract after a voltage switch (EPR must first negotiate SPR before entering EPR, slower)
#define VSET_TIMEOUT_MS         8000
#define VSET_TOLERANCE_MV       100

#define PD_VBUS_STALE_MS        1500

typedef struct parser_t
{
    uint8_t  buf[4 + FRAME_MAX_PAYLOAD + 1];
    uint8_t  pos;
    uint32_t last_byte_ms;
} parser_t;

static pd_node_t node[CH_COUNT];
static parser_t  parser[CH_COUNT];
static uint8_t   tx_seq[CH_COUNT];
static uint16_t  raw_contract_seq[CH_COUNT];
static uint8_t   raw_hard_resets[CH_COUNT];
static uint32_t  vset_start_seq[CH_COUNT];
static uint32_t  query_ms[CH_COUNT];
static uint32_t  cfg_ms[CH_COUNT];

// X035 event record: the latest PD_EVENT_LOG_LEN entries (time, channel, code, parameter), read via WCH-Link
#define PD_EVENT_LOG_LEN        16

typedef struct pd_event_t
{
    uint32_t ms;
    uint16_t arg;
    uint8_t  ch;
    uint8_t  code;
} pd_event_t;

typedef struct pd_event_log_t
{
    uint32_t   magic;           // 0x56454450 "PDEV"
    uint32_t   count;
    pd_event_t e[PD_EVENT_LOG_LEN];
} pd_event_log_t;

static pd_event_log_t pd_evt_log = {.magic = 0x56454450};

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint8_t crc8(const uint8_t *p, uint8_t len)
{
    uint8_t crc = 0;
    while (len--)
    {
        crc ^= *p++;
        for (uint8_t k = 0; k < 8; k++) crc = (uint8_t)((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

static void send(ch_id_t ch, uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t f[4 + 8 + 1];
    if (len > 8) return;
    if (++tx_seq[ch] == 0) tx_seq[ch] = 1;
    f[0] = FRAME_SOF;
    f[1] = len;
    f[2] = tx_seq[ch];
    f[3] = type;
    if (len) memcpy(&f[4], payload, len);
    f[4 + len] = crc8(&f[1], (uint8_t)(3 + len));
    uart_link_write(ch, f, (uint16_t)(5 + len));
}

static void send_target(ch_id_t ch, uint8_t mode, uint8_t flags, uint16_t mv, uint16_t ma)
{
    uint8_t p[6] = {mode, flags, (uint8_t)mv, (uint8_t)(mv >> 8), (uint8_t)ma, (uint8_t)(ma >> 8)};
    node[ch].last_cmd_code = REQ_SET_TARGET;
    node[ch].last_cmd_result = PD_RESULT_NONE;
    send(ch, REQ_SET_TARGET, p, sizeof(p));
}

// ============================================================================
// Message handling
// ============================================================================

static void handle_info(ch_id_t ch, uint8_t seq, const uint8_t *p, uint8_t len)
{
    pd_node_t *n = &node[ch];
    if (len < INFO_LEN) return;
    n->fw_major = p[0];
    n->fw_minor = p[1];
    n->fw_patch = p[2];
    n->x035_reset_flags = p[3];
    n->version_valid = true;
    n->ext_proto = n->rdo_volatile = n->fw_major >= 2;
    n->preset_mv = (p[4] != 0xFF) ? rd16(&p[6]) : 0;
    if (seq == 0) n->x035_boots++;      // unsolicited report at boot
    LOG("PD%d X035 fw %u.%u.%u rst=0x%02X target=%umV", ch + 1, n->fw_major, n->fw_minor, n->fw_patch,
        n->x035_reset_flags, n->preset_mv);
}

static void handle_status(ch_id_t ch, const uint8_t *p, uint8_t len, uint32_t now_ms)
{
    pd_node_t *n = &node[ch];
    if (len < STATUS_LEN) return;

    pd_cc_t cc = (pd_cc_t)(p[0] <= 2 ? p[0] : 0);
    if (n->cc != PD_CC_NONE && cc == PD_CC_NONE) n->detach_count++;
    n->cc = cc;

    n->pe_state = p[1];
    uint8_t f = p[2];
    n->busy = (f & 0x10) != 0;
    n->target_unmet = (f & 0x20) != 0;
    n->epr = (f & 0x02) != 0;
    n->src_epr_capable = (f & 0x08) != 0;
    n->rp = p[3];

    n->vbus_mv = rd16(&p[4]);
    n->vbus_valid = true;
    n->vbus_ms = now_ms;

    n->contract_pos = p[6];
    n->contract_kind = (pdo_kind_t)p[7];
    n->contract_mv = rd16(&p[8]);
    n->contract_ma = rd16(&p[10]);
    bool was_valid = n->contract_valid;
    n->contract_valid = (f & 0x01) && n->contract_kind != PDO_KIND_NONE && n->contract_mv;
    if (n->contract_valid) n->held_ma = n->contract_ma;
    else if (was_valid) n->lost_ms = now_ms;

    // X035 counters are 16 / 8 bit; accumulate the differences into 32 bits (counters reset to zero after an X035 restart are also handled by difference)
    uint16_t cs = rd16(&p[12]);
    if (n->status_valid && cs != raw_contract_seq[ch]) n->contract_seq += (uint16_t)(cs - raw_contract_seq[ch]);
    raw_contract_seq[ch] = cs;
    uint8_t hr = p[14];
    if (n->status_valid && hr != raw_hard_resets[ch]) n->hard_reset_count += (uint8_t)(hr - raw_hard_resets[ch]);
    raw_hard_resets[ch] = hr;

    // Capability table changed: wait for SOURCE_CAPS (sent by the X035 on its own); if missed, the periodic query fills it in
    if (p[15] != n->caps_seq && !(f & 0x40)) n->n_caps = 0;
    n->target_mode = p[18];
    n->last_result = p[19];
    n->crc_errors = rd16(&p[20]);
    n->tx_fails = rd16(&p[22]);
    n->has_auto_epr = len >= 26;
    n->epr_fail_reason = n->has_auto_epr ? p[24] : 0xFF;
    n->auto_epr = n->has_auto_epr && (p[25] & 0x01);

    n->status_valid = true;
    n->config_aligned = true;
    if (n->link != PD_LINK_ONLINE)
    {
        n->link = PD_LINK_ONLINE;
        LOG("PD%d online", ch + 1);
    }
    // Capability table sequence differs from the one received: query again
    if ((f & 0x40) && p[15] != n->caps_seq && (uint32_t)(now_ms - query_ms[ch]) >= QUERY_RETRY_MS)
    {
        query_ms[ch] = now_ms;
        send(ch, REQ_GET_SOURCE_CAPS, NULL, 0);
    }
    n->caps_seq_pending = p[15];
}

static void handle_caps(ch_id_t ch, const uint8_t *p, uint8_t len)
{
    pd_node_t *n = &node[ch];
    if (len < 2) return;
    uint8_t cnt = p[1];
    if (cnt > PD_MAX_CAPS) cnt = PD_MAX_CAPS;
    if (len < (uint8_t)(2 + 4 * cnt)) return;
    for (uint8_t i = 0; i < cnt; i++) n->caps[i] = pd_decode_pdo(rd32(&p[2 + 4 * i]));
    n->n_caps = cnt;
    n->caps_seq = n->caps_seq_pending;
}

static void dispatch(ch_id_t ch, uint8_t seq, uint8_t type, const uint8_t *p, uint8_t len, uint32_t now_ms)
{
    pd_node_t *n = &node[ch];
    n->rx_frames++;
    n->last_rx_ms = now_ms;

    switch (type)
    {
        case MSG_INFO:        handle_info(ch, seq, p, len); break;
        case MSG_STATUS:      handle_status(ch, p, len, now_ms); break;
        case MSG_SOURCE_CAPS: handle_caps(ch, p, len); break;
        case MSG_RESULT:
            if (len >= 2)
            {
                n->last_cmd_code = p[0];
                n->last_cmd_result = p[1];
                if (p[1] != PD_RESULT_OK) LOG("PD%d cmd 0x%02X -> %s", ch + 1, p[0], pd_result_str(p[1]));
            }
            break;
        case MSG_EVENT:
            if (len >= 3)
            {
                uint16_t arg = rd16(&p[1]);
                LOG("PD%d event %u arg %u", ch + 1, p[0], arg);
                pd_evt_log.e[pd_evt_log.count % PD_EVENT_LOG_LEN] =
                    (pd_event_t){.ms = now_ms, .arg = arg, .ch = (uint8_t)ch, .code = p[0]};
                pd_evt_log.count++;
                if (p[0] == PD_EVT_HARD_RESET_RX || p[0] == PD_EVT_HARD_RESET_TX)
                {
                    n->last_hr_code = p[0];
                    n->last_hr_arg = arg;
                }
            }
            break;
        default:
            break;
    }
}

static void feed(ch_id_t ch, uint8_t b, uint32_t now_ms)
{
    parser_t *ps = &parser[ch];
    // No byte for more than 20ms within a frame: discard the partial frame
    if (ps->pos && (uint32_t)(now_ms - ps->last_byte_ms) > 20) ps->pos = 0;
    ps->last_byte_ms = now_ms;

    if (ps->pos == 0 && b != FRAME_SOF) return;
    if (ps->pos == 1 && b > FRAME_MAX_PAYLOAD)
    {
        ps->pos = 0;
        node[ch].bad_frames++;
        return;
    }
    ps->buf[ps->pos++] = b;
    if (ps->pos < 4) return;
    uint8_t len = ps->buf[1];
    if (ps->pos < (uint8_t)(5 + len)) return;

    ps->pos = 0;
    if (crc8(&ps->buf[1], (uint8_t)(3 + len)) != ps->buf[4 + len])
    {
        node[ch].bad_frames++;
        return;
    }
    dispatch(ch, ps->buf[2], ps->buf[3], &ps->buf[4], len, now_ms);
}

// ============================================================================
// Voltage switching progress
// ============================================================================

static void vset_check(ch_id_t ch, uint32_t now_ms)
{
    pd_node_t *n = &node[ch];
    if (n->vset != PD_VSET_BUSY) return;

    int32_t dv = (int32_t)n->contract_mv - n->vset_target_mv;
    if (n->contract_valid && !n->busy && n->contract_seq != vset_start_seq[ch] &&
        dv >= -VSET_TOLERANCE_MV && dv <= VSET_TOLERANCE_MV)
    {
        n->vset = PD_VSET_OK;
        LOG("PD%d vset OK %umV", ch + 1, n->contract_mv);
    }
    else if (n->last_cmd_code == REQ_SET_TARGET && n->last_cmd_result == PD_RESULT_NO_MATCH)
    {
        n->vset = PD_VSET_FALLBACK;
        LOG("PD%d vset %umV: no match", ch + 1, n->vset_target_mv);
    }
    else if (n->cc == PD_CC_NONE && n->last_cmd_result != PD_RESULT_NONE)
    {
        n->vset = PD_VSET_STORED;
    }
    else if ((uint32_t)(now_ms - n->vset_start_ms) > VSET_TIMEOUT_MS)
    {
        n->vset = PD_VSET_FALLBACK;
        LOG("PD%d vset timeout: target %umV, contract %umV", ch + 1, n->vset_target_mv, n->contract_mv);
    }
}

// ============================================================================
// Public interface
// ============================================================================

void pd_node_init(void)
{
    uart_link_init();
    memset(node, 0, sizeof(node));
    memset(parser, 0, sizeof(parser));
    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        node[ch].last_cmd_result = PD_RESULT_NONE;
        query_ms[ch] = 0;
    }
}

void pd_node_process(uint32_t now_ms)
{
    uint8_t buf[64];

    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++)
    {
        pd_node_t *n = &node[ch];

        uint16_t got;
        while ((got = uart_link_read(ch, buf, sizeof(buf))) > 0)
            for (uint16_t i = 0; i < got; i++) feed(ch, buf[i], now_ms);

        if (n->link == PD_LINK_ONLINE && (uint32_t)(now_ms - n->last_rx_ms) > LINK_TIMEOUT_MS)
        {
            n->link = PD_LINK_OFFLINE;
            n->status_valid = false;
            n->contract_valid = false;
            n->held_ma = 0;
            n->vbus_valid = false;
            n->cc = PD_CC_NONE;
            n->version_valid = false;
            n->n_caps = 0;
            LOG("PD%d offline", ch + 1);
        }

        // Version or capability table unknown (main controller started later than X035): query periodically
        if ((uint32_t)(now_ms - query_ms[ch]) >= QUERY_RETRY_MS)
        {
            if (!n->version_valid)
            {
                query_ms[ch] = now_ms;
                send(ch, REQ_GET_INFO, NULL, 0);
            }
            else if (n->status_valid && n->cc != PD_CC_NONE && !n->n_caps)
            {
                query_ms[ch] = now_ms;
                send(ch, REQ_GET_SOURCE_CAPS, NULL, 0);
            }
        }

        // Auto EPR switch differs from settings (X035 defaults to on at boot): send CONFIG
        if (n->status_valid && n->has_auto_epr && n->auto_epr != settings_get()->auto_epr &&
            (uint32_t)(now_ms - cfg_ms[ch]) >= QUERY_RETRY_MS)
        {
            uint8_t flags = settings_get()->auto_epr ? 0x02 : 0;
            cfg_ms[ch] = now_ms;
            send(ch, REQ_CONFIG, &flags, 1);
        }

        vset_check(ch, now_ms);
    }
}

bool pd_node_set_voltage(ch_id_t ch, uint16_t mv)
{
    pd_node_t *n = &node[ch];
    if (!pd_node_ready(ch)) return false;

    LOG("PD%d set voltage %umV (target %umV)", ch + 1, mv, n->preset_mv);
    send_target(ch, PD_TARGET_FIXED_FIRST, TARGET_FLAG_PERSIST, mv, 0);
    n->preset_mv = mv;      // X035 has stored the boot target
    n->vset = PD_VSET_BUSY;
    n->vset_target_mv = mv;
    n->vset_start_ms = millis();
    vset_start_seq[ch] = n->contract_seq;
    return true;
}

bool pd_node_ready(ch_id_t ch)
{
    return node[ch].link == PD_LINK_ONLINE && node[ch].status_valid;
}

bool pd_node_send_pd_cmd(ch_id_t ch, uint8_t code, int32_t value)
{
    if (!pd_node_ready(ch) || code > PD_TARGET_ADJ_FIRST) return false;
    send_target(ch, code, 0, (uint16_t)((uint32_t)value & 0xFFFF), (uint16_t)((uint32_t)value >> 16));
    return true;
}

bool pd_node_hard_reset(ch_id_t ch)
{
    if (!pd_node_ready(ch)) return false;
    send(ch, REQ_HARD_RESET, NULL, 0);
    return true;
}

bool pd_node_error_recovery(ch_id_t ch, uint16_t ms)
{
    const pd_node_t *n = &node[ch];
    if (!pd_node_ready(ch) || n->fw_major < 2 || (n->fw_major == 2 && n->fw_minor < 2)) return false;
    uint8_t p[2] = {(uint8_t)ms, (uint8_t)(ms >> 8)};
    send(ch, REQ_ERROR_RECOVERY, p, sizeof(p));
    return true;
}

const char *pd_result_str(uint8_t result)
{
    switch (result)
    {
        case PD_RESULT_OK:         return "OK";
        case PD_RESULT_OK_CLAMPED: return "CLAMPED";
        case PD_RESULT_NO_SOURCE:  return "NO SOURCE";
        case PD_RESULT_NO_MATCH:   return "NO MATCH";
        case PD_RESULT_BAD_ARG:    return "BAD ARG";
        case PD_RESULT_NONE:       return "-";
        default:                   return "?";
    }
}

bool pd_node_preset_matches(ch_id_t ch, uint16_t mv)
{
    const pd_node_t *n = &node[ch];
    return n->version_valid && n->preset_mv == mv;
}

const pd_node_t *pd_node_get(ch_id_t ch)
{
    return &node[ch];
}

bool pd_node_vbus_fresh(ch_id_t ch, uint32_t now_ms)
{
    const pd_node_t *n = &node[ch];
    return n->vbus_valid && (uint32_t)(now_ms - n->vbus_ms) <= PD_VBUS_STALE_MS;
}

const char *pd_node_link_str(pd_link_t link)
{
    return link == PD_LINK_ONLINE ? "ONLINE" : "OFFLINE";
}

const char *pd_vset_str(pd_vset_t vset)
{
    switch (vset)
    {
        case PD_VSET_BUSY:     return "SWITCHING";
        case PD_VSET_OK:       return "OK";
        case PD_VSET_FALLBACK: return "FAILED";
        case PD_VSET_STORED:   return "STORED";
        default:               return "";
    }
}
