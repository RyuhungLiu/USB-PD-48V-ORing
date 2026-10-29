#pragma once

// ========================================================
//        48V10+10A ORing main controller CH32V203C8T6 pin configuration
//        Source: EasyEDA project netlist (2026-09-26)
// --------------------------------------------------------
//       LCD_RST: PA1          LM1_EN: PB9  (high = CH1 off)
//        LCD_CS: PA4          LM2_EN: PB8  (high = CH2 off)
//       LCD_SCK: PA5  SPI1     TOUCH: PB10 (TTP223, active high)
//        LCD_DC: PA6         INA_SCL: PB6  I2C1
//      LCD_MOSI: PA7  SPI1   INA_SDA: PB7  I2C1
//   PD1_TX/RX: PA2/PA3  USART2 <-> X035 #1 (CH1)
//   PD2_TX/RX: PA9/PA10 USART1 <-> X035 #2 (CH2)
//      SWDIO/SWCLK: PA13/PA14
//   LCD_BLK tied directly to 3V3 (not controllable); INA237 ALERT not connected
// ========================================================

#include <ch32v20x.h>

// ---------------- ORing path enable ----------------
#define LM_EN_GPIO_PORT     GPIOB
#define LM_EN_GPIO_CLK      RCC_APB2Periph_GPIOB
#define LM1_EN_GPIO_PIN     GPIO_Pin_9
#define LM2_EN_GPIO_PIN     GPIO_Pin_8

// LMx_EN polarity (opposite on the two hardware versions):
//   First version LM74700: LMx_EN —R101 330Ω→ Q5 gate, R14 100k pull-up to 3V3; high = off (and turning off is ineffective, output still ≈ VIN−Vth)
//   Revised LTC4359: LMx_EN —1k→ SHDN, 47k pull-down to GND; high = on
// AUTO: at power-up the pins are set as floating inputs to read the external pull-up/down and determine the version; adopted only if both channels agree, otherwise locked
// Forcing a version: locked if detection clearly shows the other version (both channels agree); if the two channels disagree, follow the forced setting
#define ORING_POL_AUTO      0
#define ORING_POL_LM74700   1
#define ORING_POL_LTC4359   2

// Revised LTC4359 board (2026-10): both channels have 47k pull-downs, AUTO identifies it correctly.
// If a first-version board is missing a pull-up on one channel (CH2 R29 was once seen ineffective), ORING_POL_LM74700 can be forced temporarily
#ifndef BOARD_ORING_POLARITY
#define BOARD_ORING_POLARITY ORING_POL_AUTO
#endif

// ---------------- ST7789 (SPI1) ----------------
#define LCD_SPI             SPI1
#define LCD_SPI_CLK         RCC_APB2Periph_SPI1
#define LCD_GPIO_PORT       GPIOA
#define LCD_GPIO_CLK        RCC_APB2Periph_GPIOA
#define LCD_RST_GPIO_PIN    GPIO_Pin_1
#define LCD_CS_GPIO_PIN     GPIO_Pin_4
#define LCD_SCK_GPIO_PIN    GPIO_Pin_5
#define LCD_DC_GPIO_PIN     GPIO_Pin_6
#define LCD_MOSI_GPIO_PIN   GPIO_Pin_7
#define LCD_DMA_TX_CH       DMA1_Channel3

// ---------------- INA237 (I2C1) ----------------
#define INA_I2C             I2C1
#define INA_I2C_CLK         RCC_APB1Periph_I2C1
#define INA_GPIO_PORT       GPIOB
#define INA_GPIO_CLK        RCC_APB2Periph_GPIOB
#define INA_SCL_GPIO_PIN    GPIO_Pin_6
#define INA_SDA_GPIO_PIN    GPIO_Pin_7
// The schematic says INA237, but the actual board reads DEVICE_ID 0x2281 = INA228; the driver adapts automatically by ID
#define INA_CH1_ADDR        0x40    // U4: A1=GND, A0=GND
#define INA_CH2_ADDR        0x44    // U6: A1=VS,  A0=GND
#define INA_SHUNT_UOHM      10000   // 10mΩ RE2512F3R010
// Current direction: CH1's IN+ is on the connector side (VIN_48V_1), CH2's IN+ is on the VIN2_ANODE side (true on both hardware versions),
// so CH2's raw reading has the opposite sign of the actual input current; software negates it so that "flowing into the board" is positive for both channels
#define INA_CH1_CURRENT_SIGN  (+1)
#define INA_CH2_CURRENT_SIGN  (-1)

// ---------------- TTP223 touch key ----------------
#define KEY_GPIO_PORT       GPIOB
#define KEY_GPIO_CLK        RCC_APB2Periph_GPIOB
#define KEY_GPIO_PIN        GPIO_Pin_10

// ---------------- X035 #1 (CH1) : USART2 ----------------
#define PD1_USART           USART2
#define PD1_USART_CLK       RCC_APB1Periph_USART2
#define PD1_GPIO_PORT       GPIOA
#define PD1_TX_GPIO_PIN     GPIO_Pin_2
#define PD1_RX_GPIO_PIN     GPIO_Pin_3
#define PD1_DMA_RX_CH       DMA1_Channel6

// ---------------- X035 #2 (CH2) : USART1 ----------------
#define PD2_USART           USART1
#define PD2_USART_CLK       RCC_APB2Periph_USART1
#define PD2_GPIO_PORT       GPIOA
#define PD2_TX_GPIO_PIN     GPIO_Pin_9
#define PD2_RX_GPIO_PIN     GPIO_Pin_10
#define PD2_DMA_RX_CH       DMA1_Channel5

#define PD_LINK_BAUDRATE    921600

// ---------------- Channel numbering ----------------
typedef enum ch_id_t
{
    CH1 = 0,
    CH2 = 1,
    CH_COUNT,
} ch_id_t;
