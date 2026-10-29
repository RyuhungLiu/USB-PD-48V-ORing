#include "uart_link.h"

typedef struct link_hw_t
{
    USART_TypeDef *usart;
    GPIO_TypeDef *port;
    uint16_t tx_pin;
    uint16_t rx_pin;
    DMA_Channel_TypeDef *dma_rx;
} link_hw_t;

static const link_hw_t hw[CH_COUNT] = {
    [CH1] = {PD1_USART, PD1_GPIO_PORT, PD1_TX_GPIO_PIN, PD1_RX_GPIO_PIN, PD1_DMA_RX_CH},
    [CH2] = {PD2_USART, PD2_GPIO_PORT, PD2_TX_GPIO_PIN, PD2_RX_GPIO_PIN, PD2_DMA_RX_CH},
};

static uint8_t  rx_buf[CH_COUNT][UART_LINK_RX_BUF_SIZE];
static uint16_t rx_tail[CH_COUNT];

static void link_init(ch_id_t ch)
{
    const link_hw_t *h = &hw[ch];

    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = h->tx_pin;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(h->port, &gpio);

    // RX floats when the X035 is unpowered or unflashed; pull-up prevents noise from being taken as data
    gpio.GPIO_Pin = h->rx_pin;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(h->port, &gpio);

    USART_InitTypeDef usart = {0};
    usart.USART_BaudRate = PD_LINK_BAUDRATE;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(h->usart, &usart);

    DMA_InitTypeDef dma = {0};
    DMA_DeInit(h->dma_rx);
    dma.DMA_PeripheralBaseAddr = (uint32_t)&h->usart->DATAR;
    dma.DMA_MemoryBaseAddr = (uint32_t)rx_buf[ch];
    dma.DMA_DIR = DMA_DIR_PeripheralSRC;
    dma.DMA_BufferSize = UART_LINK_RX_BUF_SIZE;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode = DMA_Mode_Circular;
    dma.DMA_Priority = DMA_Priority_VeryHigh;
    dma.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(h->dma_rx, &dma);
    DMA_Cmd(h->dma_rx, ENABLE);

    USART_DMACmd(h->usart, USART_DMAReq_Rx, ENABLE);
    USART_Cmd(h->usart, ENABLE);

    rx_tail[ch] = 0;
}

void uart_link_init(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | PD2_USART_CLK, ENABLE);
    RCC_APB1PeriphClockCmd(PD1_USART_CLK, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    for (ch_id_t ch = CH1; ch < CH_COUNT; ch++) link_init(ch);
}

uint16_t uart_link_read(ch_id_t ch, uint8_t *buf, uint16_t max)
{
    uint16_t head = UART_LINK_RX_BUF_SIZE - DMA_GetCurrDataCounter(hw[ch].dma_rx);
    if (head >= UART_LINK_RX_BUF_SIZE) head = 0;

    uint16_t n = 0;
    while (rx_tail[ch] != head && n < max)
    {
        buf[n++] = rx_buf[ch][rx_tail[ch]];
        rx_tail[ch] = (rx_tail[ch] + 1) % UART_LINK_RX_BUF_SIZE;
    }
    return n;
}

void uart_link_write(ch_id_t ch, const uint8_t *data, uint16_t len)
{
    USART_TypeDef *u = hw[ch].usart;
    for (uint16_t i = 0; i < len; i++)
    {
        while (USART_GetFlagStatus(u, USART_FLAG_TXE) == RESET);
        USART_SendData(u, data[i]);
    }
}

uint16_t uart_link_get_brr(ch_id_t ch)
{
    return (uint16_t)hw[ch].usart->BRR;
}

// For diagnostics: rewrite the baud rate divider directly (after transmission completes, to avoid truncating a byte being sent)
void uart_link_set_brr(ch_id_t ch, uint16_t brr)
{
    USART_TypeDef *u = hw[ch].usart;
    while (USART_GetFlagStatus(u, USART_FLAG_TC) == RESET);
    u->BRR = brr;
}
