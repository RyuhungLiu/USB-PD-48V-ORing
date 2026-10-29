#pragma once

// Wiring of the X035 (MCU / MCU1) on the 48V10+10A ORing board (checked against the schematic 2026-10-10)
//   PC14 / PC15  CC1 / CC2 (USBPD PHY, external Rd 5.1k on the board)
//   PA0          VBUS divider 150k / 10k -> ADC channel 0
//   PB0 / PB1    USART4 TX / RX -> main controller V203
//   PB3          LED (through a resistor): not driven by this firmware, kept high-impedance
//   PC16 / PC17  USB D- / D+: only used for ISP flashing, not used by this firmware
//   PA1 / PA3 and all other pins are not connected

#include "ch32x035.h"

#define BOARD_VBUS_ADC_PORT      GPIOA
#define BOARD_VBUS_ADC_PIN       GPIO_Pin_0
#define BOARD_VBUS_ADC_CHANNEL   ADC_Channel_0
#define BOARD_VBUS_DIV_NUM       16          // (150k + 10k) / 10k
#define BOARD_VREFINT_MV         1200        // ADC internal reference (used to cancel VDD error)

#define BOARD_LINK_USART         USART4
#define BOARD_LINK_USART_CLK     RCC_APB1Periph_USART4
#define BOARD_LINK_PORT          GPIOB
#define BOARD_LINK_TX_PIN        GPIO_Pin_0
#define BOARD_LINK_RX_PIN        GPIO_Pin_1
#define BOARD_LINK_BAUDRATE      921600      // must match V203 PD_LINK_BAUDRATE
#define BOARD_LINK_RX_DMA        DMA1_Channel8   // fixed mapping for USART4_RX
