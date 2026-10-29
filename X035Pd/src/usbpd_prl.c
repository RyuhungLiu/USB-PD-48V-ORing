#include "usbpd_prl.h"

#include <string.h>
#include "link.h"
#include "usbpd_phy.h"

static uint8_t tx_message_id;               // MessageIDCounter
static int8_t  rx_stored_id;                // most recently received MessageID, -1 = none
static uint8_t spec_rev = PD_REV_30;

// Chunked reception
static struct
{
    bool     active;
    uint8_t  type;
    uint8_t  next_chunk;
    uint16_t size;
    uint16_t got;
    uint32_t deadline;
    uint8_t  spec_rev;
    uint8_t  buf[PRL_EXT_BUF_BYTES];
} chunk;

void prl_reset(void)
{
    tx_message_id = 0;
    rx_stored_id = -1;
    chunk.active = false;
}

void prl_set_spec_rev(uint8_t rev)
{
    spec_rev = (rev >= PD_REV_30) ? PD_REV_30 : PD_REV_20;
}

uint8_t prl_spec_rev(void)
{
    return spec_rev;
}

// Header: Sink, UFP
static uint16_t make_header(uint8_t type, uint8_t num_do, bool ext)
{
    return (uint16_t)((type & 0x1F) | (spec_rev << 6) | ((tx_message_id & 7) << 9) |
                      ((num_do & 7) << 12) | (ext ? 0x8000 : 0));
}

// PRL_Tx: send + nRetryCount retries; MessageIDCounter increments whether or not it succeeds (6.12.2.2 Transmission Error)
static bool tx(uint8_t *buf, uint8_t len)
{
    bool ok = false;
    for (uint8_t i = 0; i <= PD_nRetryCount && !ok; i++) ok = phy_transmit(buf, len, tx_message_id);
    link_forward_pd(1, buf, len);
    tx_message_id = (uint8_t)((tx_message_id + 1) & 7);
    return ok;
}

bool prl_tx_ctrl(pd_ctrl_msg_t type)
{
    uint8_t buf[2];
    uint16_t h = make_header((uint8_t)type, 0, false);
    buf[0] = (uint8_t)h;
    buf[1] = (uint8_t)(h >> 8);
    return tx(buf, 2);
}

bool prl_tx_data(pd_data_msg_t type, const uint32_t *obj, uint8_t n)
{
    uint8_t buf[2 + 4 * PD_MAX_DATA_OBJECTS];
    if (n > PD_MAX_DATA_OBJECTS) n = PD_MAX_DATA_OBJECTS;
    uint16_t h = make_header((uint8_t)type, n, false);
    buf[0] = (uint8_t)h;
    buf[1] = (uint8_t)(h >> 8);
    for (uint8_t i = 0; i < n; i++)
    {
        buf[2 + 4 * i] = (uint8_t)obj[i];
        buf[3 + 4 * i] = (uint8_t)(obj[i] >> 8);
        buf[4 + 4 * i] = (uint8_t)(obj[i] >> 16);
        buf[5 + 4 * i] = (uint8_t)(obj[i] >> 24);
    }
    return tx(buf, (uint8_t)(2 + 4 * n));
}

// Extended message (Chunked, single chunk): header + extended header + data, zero-padded to 4-byte alignment
static bool tx_ext_raw(uint8_t type, uint16_t ext_header, const uint8_t *data, uint8_t size)
{
    uint8_t buf[2 + 2 + PD_MAX_EXT_CHUNK_BYTES + 2];
    uint8_t body = (uint8_t)(2 + size);
    uint8_t num_do = (uint8_t)((body + 3) / 4);
    uint16_t h = make_header(type, num_do, true);
    memset(buf, 0, sizeof(buf));
    buf[0] = (uint8_t)h;
    buf[1] = (uint8_t)(h >> 8);
    buf[2] = (uint8_t)ext_header;
    buf[3] = (uint8_t)(ext_header >> 8);
    if (size) memcpy(&buf[4], data, size);
    return tx(buf, (uint8_t)(2 + 4 * num_do));
}

bool prl_tx_ext(pd_ext_msg_t type, const uint8_t *data, uint8_t size)
{
    if (size > PD_MAX_EXT_CHUNK_BYTES) size = PD_MAX_EXT_CHUNK_BYTES;
    return tx_ext_raw((uint8_t)type, PD_EXT_HEADER(size, 0, 0, 1), data, size);
}

bool prl_tx_ext_ctrl(pd_ext_ctrl_t type, uint8_t data)
{
    uint8_t ecdb[2] = {(uint8_t)type, data};
    return prl_tx_ext(PD_EXT_Extended_Control, ecdb, sizeof(ecdb));
}

void prl_tx_hard_reset(void)
{
    phy_transmit_hard_reset();
    prl_reset();
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void fill_common(prl_msg_t *m, uint16_t h)
{
    memset(m, 0, sizeof(*m));
    m->header = h;
    m->type = PD_HEADER_TYPE(h);
    m->num_do = PD_HEADER_NUM_DO(h);
    m->spec_rev = (uint8_t)PD_HEADER_SPEC_REV(h);
    m->extended = PD_HEADER_EXTENDED(h);
}

bool prl_rx(prl_msg_t *msg, uint32_t now_ms)
{
    phy_rx_msg_t raw;

    if (chunk.active && (int32_t)(now_ms - chunk.deadline) > 0) chunk.active = false;

    while (phy_rx_pop(&raw))
    {
        if (raw.len < 2) continue;
        link_forward_pd(0, raw.buf, raw.len);
        uint16_t h = (uint16_t)(raw.buf[0] | (raw.buf[1] << 8));
        uint8_t type = PD_HEADER_TYPE(h);
        uint8_t id = PD_HEADER_MESSAGE_ID(h);
        bool ext = PD_HEADER_EXTENDED(h);

        // Soft_Reset: reset counters, then hand over to the policy engine
        if (!ext && PD_HEADER_NUM_DO(h) == 0 && type == PD_CTRL_Soft_Reset)
        {
            prl_reset();
            rx_stored_id = (int8_t)id;
            fill_common(msg, h);
            return true;
        }
        // Duplicate (partner retransmitted because it did not receive our GoodCRC)
        if (rx_stored_id == (int8_t)id) continue;
        rx_stored_id = (int8_t)id;

        if (!ext)
        {
            fill_common(msg, h);
            uint8_t n = msg->num_do;
            if (n > PD_MAX_DATA_OBJECTS) n = PD_MAX_DATA_OBJECTS;
            if ((uint8_t)(2 + 4 * n) > raw.len) n = (uint8_t)((raw.len - 2) / 4);
            for (uint8_t i = 0; i < n; i++) msg->obj[i] = rd32(&raw.buf[2 + 4 * i]);
            msg->num_do = n;
            return true;
        }

        if (raw.len < 4) continue;
        uint16_t eh = (uint16_t)(raw.buf[2] | (raw.buf[3] << 8));
        uint16_t size = PD_EXT_HEADER_DATA_SIZE(eh);
        uint8_t  num = PD_EXT_HEADER_CHUNK_NUM(eh);
        uint8_t  avail = (uint8_t)(raw.len - 4);

        // Chunk Request from the partner (we never send multi-chunk messages): ignore
        if (PD_EXT_HEADER_REQUEST(eh)) continue;
        // Unchunked extended messages are not supported
        if (!PD_EXT_HEADER_CHUNKED(eh)) continue;

        if (num == 0)
        {
            chunk.active = false;
            if (size > PRL_EXT_BUF_BYTES) continue;
            uint8_t take = (uint8_t)(size < PD_MAX_EXT_CHUNK_BYTES ? size : PD_MAX_EXT_CHUNK_BYTES);
            if (take > avail) take = avail;
            if (size <= take)
            {
                fill_common(msg, h);
                msg->ext_size = size;
                memcpy(msg->ext, &raw.buf[4], size);
                return true;
            }
            chunk.active = true;
            chunk.type = type;
            chunk.size = size;
            chunk.got = take;
            chunk.next_chunk = 1;
            chunk.spec_rev = (uint8_t)PD_HEADER_SPEC_REV(h);
            memcpy(chunk.buf, &raw.buf[4], take);
        }
        else
        {
            if (!chunk.active || type != chunk.type || num != chunk.next_chunk) continue;
            uint16_t remain = (uint16_t)(chunk.size - chunk.got);
            uint8_t take = (uint8_t)(remain < PD_MAX_EXT_CHUNK_BYTES ? remain : PD_MAX_EXT_CHUNK_BYTES);
            if (take > avail) take = avail;
            memcpy(&chunk.buf[chunk.got], &raw.buf[4], take);
            chunk.got = (uint16_t)(chunk.got + take);
            chunk.next_chunk++;
            if (chunk.got >= chunk.size)
            {
                chunk.active = false;
                fill_common(msg, h);
                msg->ext_size = chunk.size;
                memcpy(msg->ext, chunk.buf, chunk.size);
                return true;
            }
        }

        // Request the next chunk (within tChunkSenderRequest)
        chunk.deadline = now_ms + PD_tChunkSenderRequest * 2;
        tx_ext_raw(chunk.type, PD_EXT_HEADER(0, 1, chunk.next_chunk, 1), NULL, 0);
    }
    return false;
}
