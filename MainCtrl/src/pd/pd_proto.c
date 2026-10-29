#include "pd_proto.h"

#include <string.h>

#define HDR_MINI        0xA2
#define HDR_STD         0xA1
#define HDR_1           0xFF
#define MINI_LEN_OFS    2
#define STD_LEN_OFS     10

// An intra-packet byte gap longer than this is treated as a truncated packet; discard and resynchronize
#define FRAME_GAP_MS    5

void pd_parser_reset(pd_parser_t *p)
{
    p->pos = 0;
    p->expected = 0;
}

bool pd_parser_feed(pd_parser_t *p, uint8_t byte, uint32_t now_ms, pd_frame_t *out)
{
    if (p->pos > 0 && (uint32_t)(now_ms - p->last_byte_ms) > FRAME_GAP_MS)
    {
        p->bad_frames++;
        pd_parser_reset(p);
    }
    p->last_byte_ms = now_ms;

    if (p->pos == 0)
    {
        if (byte == HDR_MINI || byte == HDR_STD) p->buf[p->pos++] = byte;
        return false;
    }

    if (p->pos == 1)
    {
        if (byte != HDR_1)
        {
            pd_parser_reset(p);
            // This byte may be the header of the next packet
            if (byte == HDR_MINI || byte == HDR_STD) p->buf[p->pos++] = byte;
            return false;
        }
        p->buf[p->pos++] = byte;
        return false;
    }

    p->buf[p->pos++] = byte;

    if (p->expected == 0)
    {
        bool mini = (p->buf[0] == HDR_MINI);
        uint8_t len_ofs = mini ? MINI_LEN_OFS : STD_LEN_OFS;
        if (p->pos > len_ofs)
        {
            uint8_t len = p->buf[len_ofs];
            uint16_t total = (uint16_t)(len_ofs + 1) + len;
            if (len == 0 || len > PD_PROTO_MAX_DATA + 1 || total > sizeof(p->buf))
            {
                p->bad_frames++;
                pd_parser_reset(p);
                return false;
            }
            p->expected = (uint8_t)total;
        }
    }

    if (p->expected == 0 || p->pos < p->expected) return false;

    uint8_t len_ofs = (p->buf[0] == HDR_MINI) ? MINI_LEN_OFS : STD_LEN_OFS;
    out->type = p->buf[len_ofs + 1];
    out->len = p->buf[len_ofs] - 1;
    memcpy(out->data, &p->buf[len_ofs + 2], out->len);

    pd_parser_reset(p);
    return true;
}

uint8_t pd_build_cmd(uint8_t *out, uint8_t category, uint8_t cmd, int32_t value)
{
    out[0] = 0x52;
    out[1] = 0xFF;
    out[2] = 7;                 // Type + Category + Cmd + Value(4)
    out[3] = PD_REPORT_SYSTEM;  // commands use the SYS data channel; Category distinguishes PD/SYS
    out[4] = category;
    out[5] = cmd;
    out[6] = (uint8_t)(value);
    out[7] = (uint8_t)(value >> 8);
    out[8] = (uint8_t)(value >> 16);
    out[9] = (uint8_t)(value >> 24);
    out[10] = 0x00;             // padding, see pd_proto.h
    return PD_PROTO_CMD_LEN;
}
