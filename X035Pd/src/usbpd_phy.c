#include "usbpd_phy.h"

#include <string.h>
#include "ch32x035.h"
#include "ch32x035_usbpd.h"
#include "sys_time.h"
#include "usbpd_def.h"

// 0 = software CRC is only counted, not dropped (hardware reception result is authoritative); can be changed to 1 once the count is confirmed to stay at 0 long-term
#ifndef PHY_RX_CRC_CHECK
#define PHY_RX_CRC_CHECK    0
#endif

#define RX_QUEUE_LEN        4

void USBPD_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

static __attribute__((aligned(4))) uint8_t rx_dma[PHY_MSG_MAX_BYTES];
static __attribute__((aligned(4))) uint8_t tx_dma[PHY_MSG_MAX_BYTES];
static __attribute__((aligned(4))) uint8_t ack_dma[4];

static phy_rx_msg_t     rx_queue[RX_QUEUE_LEN];
static volatile uint8_t rx_head, rx_tail;
static volatile bool    hard_reset_flag;
static uint8_t          active_cc;          // 0 = not attached
static phy_stats_t      stats;

// ---------------------------------------------------------------------------
// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320), placed little-endian after the data in the message (inverted value)
// ---------------------------------------------------------------------------
static uint32_t crc32(const uint8_t *p, uint8_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (len--)
    {
        crc ^= *p++;
        for (uint8_t k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

static volatile uint16_t *cc_port(uint8_t cc)
{
    return (cc == 2) ? &USBPD->PORT_CC2 : &USBPD->PORT_CC1;
}

static void rx_arm(void)
{
    USBPD->CONFIG |= PD_ALL_CLR;
    USBPD->CONFIG &= ~PD_ALL_CLR;
    USBPD->CONFIG |= IE_RX_ACT | IE_RX_RESET | PD_DMA_EN;
    USBPD->DMA = (uint16_t)(uint32_t)rx_dma;
    USBPD->CONTROL &= ~PD_TX_EN;
    USBPD->BMC_CLK_CNT = UPD_TMR_RX_48M;
    USBPD->CONTROL |= BMC_START;
}

// Transmit (blocks until TX_END), then switch back to receive
static void tx_raw(const uint8_t *buf, uint8_t len, uint8_t tx_sel)
{
    volatile uint16_t *port = cc_port(active_cc);
    *port |= CC_LVE;
    USBPD->BMC_CLK_CNT = UPD_TMR_TX_48M;
    USBPD->DMA = (uint16_t)(uint32_t)buf;
    USBPD->TX_SEL = tx_sel;
    USBPD->BMC_TX_SZ = len;
    USBPD->CONTROL |= PD_TX_EN;
    USBPD->STATUS &= BMC_AUX_INVALID;
    USBPD->CONTROL |= BMC_START;
    while ((USBPD->STATUS & IF_TX_END) == 0) {}
    USBPD->STATUS = IF_TX_END;
    *port &= ~CC_LVE;
    rx_arm();
}

void phy_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBPD, ENABLE);

    gpio.GPIO_Pin = GPIO_Pin_14 | GPIO_Pin_15;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &gpio);

    // VDD = 3.3V: PHY uses VDD directly; PD pins as high-threshold inputs
    AFIO->CTLR |= USBPD_IN_HVT | USBPD_PHY_V33;

    USBPD->CONFIG = PD_DMA_EN;
    USBPD->STATUS = BUF_ERR | IF_RX_BIT | IF_RX_BYTE | IF_RX_ACT | IF_RX_RESET | IF_TX_END;
    // Sink: both CC lines marked as pull-down (Rd provided by the board resistor), receive comparator 0.66V
    USBPD->PORT_CC1 = CC_CMP_66 | CC_PD;
    USBPD->PORT_CC2 = CC_CMP_66 | CC_PD;

    NVIC_SetPriority(USBPD_IRQn, 0);       // highest priority: GoodCRC must go out within tTransmit
    active_cc = 0;
}

uint8_t phy_cc_detect(void)
{
    uint8_t res = 0;
    for (uint8_t cc = 1; cc <= 2; cc++)
    {
        volatile uint16_t *port = cc_port(cc);
        *port = (uint16_t)((*port & ~(CC_CMP_Mask | PA_CC_AI)) | CC_CMP_22);
        sys_delay_us(2);
        if (*port & PA_CC_AI) res |= (uint8_t)(1u << (cc - 1));
        *port = (uint16_t)((*port & ~CC_CMP_Mask) | CC_CMP_66);
    }
    return res;
}

void phy_attach(uint8_t cc)
{
    active_cc = cc;
    if (cc == 2) USBPD->CONFIG |= CC_SEL;
    else USBPD->CONFIG &= ~CC_SEL;
    rx_head = rx_tail = 0;
    hard_reset_flag = false;
    rx_arm();
    NVIC_EnableIRQ(USBPD_IRQn);
}

void phy_detach(void)
{
    NVIC_DisableIRQ(USBPD_IRQn);
    USBPD->CONTROL &= ~PD_TX_EN;
    USBPD->CONFIG &= ~(IE_RX_ACT | IE_RX_RESET | IE_TX_END);
    active_cc = 0;
    rx_head = rx_tail = 0;
}

void phy_cc_open(void)
{
    USBPD->PORT_CC1 = 0;
    USBPD->PORT_CC2 = 0;
    GPIO_SetBits(GPIOC, GPIO_Pin_14 | GPIO_Pin_15);
    GPIO_InitTypeDef gpio = {.GPIO_Pin = GPIO_Pin_14 | GPIO_Pin_15, .GPIO_Speed = GPIO_Speed_50MHz, .GPIO_Mode = GPIO_Mode_Out_PP};
    GPIO_Init(GPIOC, &gpio);
}

void phy_cc_release(void)
{
    GPIO_InitTypeDef gpio = {.GPIO_Pin = GPIO_Pin_14 | GPIO_Pin_15, .GPIO_Speed = GPIO_Speed_50MHz, .GPIO_Mode = GPIO_Mode_IN_FLOATING};
    GPIO_Init(GPIOC, &gpio);
    GPIO_ResetBits(GPIOC, GPIO_Pin_14 | GPIO_Pin_15);
    USBPD->PORT_CC1 = CC_CMP_66 | CC_PD;
    USBPD->PORT_CC2 = CC_CMP_66 | CC_PD;
}

phy_rp_t phy_rp_level(void)
{
    if (!active_cc) return PHY_RP_NONE;
    volatile uint16_t *port = cc_port(active_cc);
    uint8_t hi123 = 0, hi66 = 0, hi22 = 0;

    // CC level toggles during BMC communication: sample several times, take "always above threshold"
    NVIC_DisableIRQ(USBPD_IRQn);
    for (uint8_t i = 0; i < 4; i++)
    {
        *port = (uint16_t)((*port & ~(CC_CMP_Mask | PA_CC_AI)) | CC_CMP_123);
        sys_delay_us(2);
        hi123 += (*port & PA_CC_AI) ? 1 : 0;
        *port = (uint16_t)((*port & ~(CC_CMP_Mask | PA_CC_AI)) | CC_CMP_66);
        sys_delay_us(2);
        hi66 += (*port & PA_CC_AI) ? 1 : 0;
        *port = (uint16_t)((*port & ~(CC_CMP_Mask | PA_CC_AI)) | CC_CMP_22);
        sys_delay_us(2);
        hi22 += (*port & PA_CC_AI) ? 1 : 0;
    }
    *port = (uint16_t)((*port & ~CC_CMP_Mask) | CC_CMP_66);
    NVIC_EnableIRQ(USBPD_IRQn);

    if (hi123 == 4) return PHY_RP_3A0;
    if (hi66 == 4) return PHY_RP_1A5;
    if (hi22 == 4) return PHY_RP_DEFAULT;
    return PHY_RP_NONE;
}

bool phy_rx_pop(phy_rx_msg_t *msg)
{
    if (rx_tail == rx_head) return false;
    *msg = rx_queue[rx_tail];
    rx_tail = (uint8_t)((rx_tail + 1) % RX_QUEUE_LEN);
    return true;
}

bool phy_hard_reset_received(void)
{
    if (!hard_reset_flag) return false;
    hard_reset_flag = false;
    return true;
}

bool phy_transmit(const uint8_t *buf, uint8_t len, uint8_t message_id)
{
    if (!active_cc || len > PHY_MSG_MAX_BYTES - 4) return false;
    memcpy(tx_dma, buf, len);

    NVIC_DisableIRQ(USBPD_IRQn);
    tx_raw(tx_dma, len, UPD_SOP0);

    // Wait for GoodCRC within tReceive
    bool ok = false;
    uint32_t t0 = sys_time_us();
    while ((uint32_t)(sys_time_us() - t0) < PD_tReceive_us)
    {
        if (!(USBPD->STATUS & IF_RX_ACT)) continue;
        uint8_t st = USBPD->STATUS;
        uint16_t cnt = USBPD->BMC_BYTE_CNT;
        USBPD->STATUS = IF_RX_ACT;
        if ((st & MASK_PD_STAT) == PD_RX_SOP0 && cnt == 6)
        {
            uint16_t h = (uint16_t)(rx_dma[0] | (rx_dma[1] << 8));
            if (PD_HEADER_TYPE(h) == PD_CTRL_GoodCRC && PD_HEADER_NUM_DO(h) == 0 &&
                PD_HEADER_MESSAGE_ID(h) == message_id)
            {
                ok = true;
                break;
            }
        }
        rx_arm();   // other message (collision): no GoodCRC reply, the partner will retransmit
    }
    rx_arm();
    if (!ok) stats.tx_no_goodcrc++;
    if (USBPD->STATUS & IF_RX_RESET)
    {
        USBPD->STATUS = IF_RX_RESET;
        hard_reset_flag = true;
        stats.hard_resets_rx++;
    }
    NVIC_EnableIRQ(USBPD_IRQn);
    return ok;
}

void phy_transmit_hard_reset(void)
{
    if (!active_cc) return;
    NVIC_DisableIRQ(USBPD_IRQn);
    tx_raw(tx_dma, 0, UPD_HARD_RESET);
    NVIC_EnableIRQ(USBPD_IRQn);
}

const phy_stats_t *phy_stats(void)
{
    return &stats;
}

// ---------------------------------------------------------------------------
// Receive interrupt: SOP message -> check -> reply GoodCRC -> enqueue; Hard Reset -> set flag
// ---------------------------------------------------------------------------
void USBPD_IRQHandler(void)
{
    uint8_t st = USBPD->STATUS;

    if (st & IF_RX_ACT)
    {
        uint16_t cnt = USBPD->BMC_BYTE_CNT;
        USBPD->STATUS = IF_RX_ACT;

        if ((st & MASK_PD_STAT) == PD_RX_SOP0 && cnt >= 6 && cnt <= PHY_MSG_MAX_BYTES)
        {
            uint8_t len = (uint8_t)(cnt - 4);
            uint16_t h = (uint16_t)(rx_dma[0] | (rx_dma[1] << 8));
            uint32_t rx_crc = (uint32_t)rx_dma[len] | ((uint32_t)rx_dma[len + 1] << 8) |
                              ((uint32_t)rx_dma[len + 2] << 16) | ((uint32_t)rx_dma[len + 3] << 24);
            bool crc_ok = crc32(rx_dma, len) == rx_crc;
            if (!crc_ok) stats.rx_crc_errors++;

            bool is_goodcrc = PD_HEADER_TYPE(h) == PD_CTRL_GoodCRC && PD_HEADER_NUM_DO(h) == 0 && !PD_HEADER_EXTENDED(h);
            if ((crc_ok || !PHY_RX_CRC_CHECK) && !is_goodcrc)
            {
                // Enqueue (if full, drop the newest one: no GoodCRC reply, the partner will retransmit)
                uint8_t next = (uint8_t)((rx_head + 1) % RX_QUEUE_LEN);
                if (next != rx_tail)
                {
                    rx_queue[rx_head].len = len;
                    memcpy(rx_queue[rx_head].buf, rx_dma, len);

                    // GoodCRC: Sink / UFP, revision copied from the received message, MessageID echoed
                    sys_delay_us(30);   // tInterFrameGap
                    uint16_t ack = (uint16_t)(PD_CTRL_GoodCRC | (PD_HEADER_SPEC_REV(h) << 6) |
                                              (PD_HEADER_MESSAGE_ID(h) << 9));
                    ack_dma[0] = (uint8_t)ack;
                    ack_dma[1] = (uint8_t)(ack >> 8);
                    tx_raw(ack_dma, 2, UPD_SOP0);

                    rx_head = next;
                    stats.rx_msgs++;
                    return;
                }
                stats.rx_overflows++;
            }
        }
        rx_arm();
    }

    if (st & IF_RX_RESET)
    {
        USBPD->STATUS = IF_RX_RESET;
        hard_reset_flag = true;
        stats.hard_resets_rx++;
        rx_arm();
    }

    if (st & BUF_ERR)
    {
        USBPD->STATUS = BUF_ERR;
        rx_arm();
    }
}
