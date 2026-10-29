#include "link.h"

#include <string.h>
#include "board.h"
#include "config.h"
#include "nv.h"
#include "sys_time.h"
#include "usbpd_pe.h"
#include "usbpd_phy.h"
#include "usbpd_tc.h"
#include "vbus.h"

#define RX_DMA_SIZE         256
#define STATUS_PERIOD_MS    500

static uint8_t  rx_dma[RX_DMA_SIZE];
static uint16_t rx_pos;                 // next position to read

// Frame parsing
static uint8_t  frame[4 + LINK_MAX_PAYLOAD + 1];
static uint8_t  frame_len;              // bytes received so far
static bool     forward_pd;

static uint32_t last_status_ms;
static uint32_t last_status_rev;
static uint8_t  last_caps_seq;

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

static void put_byte(uint8_t b)
{
    while (USART_GetFlagStatus(BOARD_LINK_USART, USART_FLAG_TXE) == RESET) {}
    USART_SendData(BOARD_LINK_USART, b);
}

void link_send(uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len)
{
    if (len > LINK_MAX_PAYLOAD) len = LINK_MAX_PAYLOAD;
    uint8_t hdr[4] = {0xA5, len, seq, type};
    uint8_t crc = crc8(&hdr[1], 3);
    // Continue the CRC: payload follows the header
    for (uint8_t i = 0; i < len; i++)
    {
        crc ^= payload[i];
        for (uint8_t k = 0; k < 8; k++) crc = (uint8_t)((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
    }
    for (uint8_t i = 0; i < 4; i++) put_byte(hdr[i]);
    for (uint8_t i = 0; i < len; i++) put_byte(payload[i]);
    put_byte(crc);
}

void link_send_event(link_event_t code, uint16_t arg)
{
    uint8_t p[3] = {(uint8_t)code, (uint8_t)arg, (uint8_t)(arg >> 8)};
    link_send(LINK_MSG_EVENT, 0, p, sizeof(p));
}

void link_forward_pd(uint8_t dir, const uint8_t *buf, uint8_t len)
{
    if (!forward_pd) return;
    uint8_t p[1 + 30];
    if (len > 30) len = 30;
    p[0] = dir;
    memcpy(&p[1], buf, len);
    link_send(LINK_MSG_PD_MSG, 0, p, (uint8_t)(len + 1));
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void send_status(uint8_t seq)
{
    const pe_status_t *s = pe_status();
    const phy_stats_t *ps = phy_stats();
    uint8_t p[26];

    p[0] = tc_attached_cc();
    p[1] = (uint8_t)s->state;
    p[2] = (uint8_t)((s->explicit_contract ? 0x01 : 0) | (s->epr_mode ? 0x02 : 0) |
                     (s->spec_rev >= 2 ? 0x04 : 0) | (s->src_epr_capable ? 0x08 : 0) |
                     (s->busy ? 0x10 : 0) | (s->target_unmet ? 0x20 : 0) | (s->n_caps ? 0x40 : 0));
    p[3] = (uint8_t)s->rp;
    put16(&p[4], vbus_mv());
    p[6] = s->contract.pos;
    p[7] = (uint8_t)s->contract.kind;
    put16(&p[8], s->contract.mv);
    put16(&p[10], s->contract.ma);
    put16(&p[12], s->contract_seq);
    p[14] = s->hard_resets;
    p[15] = s->caps_seq;
    put16(&p[16], s->target.mv);
    p[18] = s->target.mode;
    p[19] = s->last_result;
    put16(&p[20], ps->rx_crc_errors);
    put16(&p[22], ps->tx_no_goodcrc);
    p[24] = s->epr_fail_reason;
    p[25] = s->auto_epr ? 0x01 : 0;
    link_send(LINK_MSG_STATUS, seq, p, sizeof(p));
}

static void send_caps(uint8_t seq)
{
    const pe_status_t *s = pe_status();
    uint8_t p[2 + 4 * PE_MAX_CAPS];
    p[0] = s->caps_epr ? 1 : 0;
    p[1] = s->n_caps;
    for (uint8_t i = 0; i < s->n_caps; i++)
    {
        uint32_t v = s->caps[i];
        p[2 + 4 * i] = (uint8_t)v;
        p[3 + 4 * i] = (uint8_t)(v >> 8);
        p[4 + 4 * i] = (uint8_t)(v >> 16);
        p[5 + 4 * i] = (uint8_t)(v >> 24);
    }
    link_send(LINK_MSG_SOURCE_CAPS, seq, p, (uint8_t)(2 + 4 * s->n_caps));
}

static void send_info(uint8_t seq)
{
    const nv_target_t *t = nv_target();
    uint8_t p[10];
    p[0] = FW_VERSION_MAJOR;
    p[1] = FW_VERSION_MINOR;
    p[2] = FW_VERSION_PATCH;
    p[3] = (uint8_t)(RCC->RSTSCKR >> 24);
    p[4] = t ? t->mode : 0xFF;
    p[5] = t ? t->flags : 0;
    put16(&p[6], t ? t->mv : 0);
    put16(&p[8], t ? t->ma : 0);
    link_send(LINK_MSG_INFO, seq, p, sizeof(p));
}

static void send_result(uint8_t seq, uint8_t type, uint8_t code)
{
    uint8_t p[2] = {type, code};
    link_send(LINK_MSG_RESULT, seq, p, sizeof(p));
}

static void handle(uint8_t seq, uint8_t type, const uint8_t *p, uint8_t len)
{
    switch (type)
    {
        case LINK_REQ_GET_STATUS:
            send_status(seq);
            break;

        case LINK_REQ_SET_TARGET:
        {
            if (len < 6 || p[0] > 1)
            {
                send_result(seq, type, LINK_RESULT_BAD_ARG);
                break;
            }
            pe_target_t t = {.mode = p[0], .flags = p[1],
                             .mv = (uint16_t)(p[2] | (p[3] << 8)), .ma = (uint16_t)(p[4] | (p[5] << 8))};
            send_result(seq, type, (uint8_t)pe_set_target(&t));
            break;
        }

        case LINK_REQ_GET_SOURCE_CAPS:
            send_caps(seq);
            break;

        case LINK_REQ_HARD_RESET:
            pe_request_hard_reset();
            send_result(seq, type, LINK_RESULT_OK);
            break;

        case LINK_REQ_CONFIG:
            forward_pd = len >= 1 && (p[0] & 0x01);
            pe_set_auto_epr(len >= 1 && (p[0] & 0x02));
            send_result(seq, type, LINK_RESULT_OK);
            break;

        case LINK_REQ_REBOOT:
            send_result(seq, type, LINK_RESULT_OK);
            sys_delay_us(2000);
            NVIC_SystemReset();
            break;

        case LINK_REQ_GET_INFO:
            send_info(seq);
            break;

        case LINK_REQ_ERROR_RECOVERY:
        {
            uint16_t ms = (len >= 2) ? (uint16_t)(p[0] | (p[1] << 8)) : 500;
            if (ms > 5000) ms = 5000;
            tc_request_error_recovery(ms);
            send_result(seq, type, LINK_RESULT_OK);
            break;
        }

        default:
            send_result(seq, type, LINK_RESULT_BAD_ARG);
            break;
    }
}

static void feed(uint8_t b)
{
    if (frame_len == 0 && b != 0xA5) return;
    if (frame_len == 1 && b > LINK_MAX_PAYLOAD)
    {
        frame_len = 0;
        return;
    }
    frame[frame_len++] = b;
    if (frame_len < 4) return;
    uint8_t len = frame[1];
    if (frame_len < (uint8_t)(4 + len + 1)) return;

    frame_len = 0;
    if (crc8(&frame[1], (uint8_t)(3 + len)) != frame[4 + len]) return;
    handle(frame[2], frame[3], &frame[4], len);
}

void link_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    USART_InitTypeDef usart = {0};
    DMA_InitTypeDef dma = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    RCC_APB1PeriphClockCmd(BOARD_LINK_USART_CLK, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    gpio.GPIO_Pin = BOARD_LINK_TX_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(BOARD_LINK_PORT, &gpio);
    gpio.GPIO_Pin = BOARD_LINK_RX_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(BOARD_LINK_PORT, &gpio);

    usart.USART_BaudRate = BOARD_LINK_BAUDRATE;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(BOARD_LINK_USART, &usart);

    // Receive via DMA ring buffer: the USBPD interrupt blocks about 0.5ms when replying GoodCRC; UART bytes are not lost meanwhile
    DMA_DeInit(BOARD_LINK_RX_DMA);
    dma.DMA_PeripheralBaseAddr = (uint32_t)&BOARD_LINK_USART->DATAR;
    dma.DMA_MemoryBaseAddr = (uint32_t)rx_dma;
    dma.DMA_DIR = DMA_DIR_PeripheralSRC;
    dma.DMA_BufferSize = RX_DMA_SIZE;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode = DMA_Mode_Circular;
    dma.DMA_Priority = DMA_Priority_High;
    dma.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(BOARD_LINK_RX_DMA, &dma);
    DMA_Cmd(BOARD_LINK_RX_DMA, ENABLE);
    USART_DMACmd(BOARD_LINK_USART, USART_DMAReq_Rx, ENABLE);
    USART_Cmd(BOARD_LINK_USART, ENABLE);

    rx_pos = 0;
    send_info(0);
}

void link_process(uint32_t now_ms)
{
    uint16_t head = (uint16_t)(RX_DMA_SIZE - DMA_GetCurrDataCounter(BOARD_LINK_RX_DMA));
    if (head >= RX_DMA_SIZE) head = 0;
    while (rx_pos != head)
    {
        feed(rx_dma[rx_pos]);
        rx_pos = (uint16_t)((rx_pos + 1) % RX_DMA_SIZE);
    }

    // New source capabilities are reported proactively; status changes are reported immediately, otherwise periodically (doubles as heartbeat)
    const pe_status_t *s = pe_status();
    if (s->caps_seq != last_caps_seq)
    {
        last_caps_seq = s->caps_seq;
        send_caps(0);
    }
    if (s->rev != last_status_rev || (uint32_t)(now_ms - last_status_ms) >= STATUS_PERIOD_MS)
    {
        last_status_rev = s->rev;
        last_status_ms = now_ms;
        send_status(0);
    }
}
