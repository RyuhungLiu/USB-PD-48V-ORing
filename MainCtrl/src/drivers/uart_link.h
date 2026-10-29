#pragma once

#include <stdint.h>
#include "board.h"

// Main controller <-> X035 UART link, 921600 8N1
//   CH1: USART2 (PA2/PA3)  RX DMA1_Ch6
//   CH2: USART1 (PA9/PA10) RX DMA1_Ch5
// Receive: DMA circular buffer, polled and drained by the main loop; transmit: blocking (single packet <=64B ~ 0.7ms)

// At boot the X035 and the main controller power up together; PD negotiation (including a dozen or so EPR messages, each <=55B) finishes before the main controller
// enters the main loop, so these messages accumulate in the DMA buffer first; it must hold all forwarded traffic of the boot phase (about 0.6s)
#define UART_LINK_RX_BUF_SIZE 2048

void uart_link_init(void);

// Take out the received bytes, returns the byte count
uint16_t uart_link_read(ch_id_t ch, uint8_t *buf, uint16_t max);

void uart_link_write(ch_id_t ch, const uint8_t *data, uint16_t len);

// For diagnostics: read/rewrite BRR (USART1 clock 144MHz, USART2 clock 72MHz)
uint16_t uart_link_get_brr(ch_id_t ch);
void uart_link_set_brr(ch_id_t ch, uint16_t brr);
