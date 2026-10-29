#include "st7789.h"

#include <stdbool.h>
#include <stddef.h>
#include "board.h"
#include "bsp/delay.h"

#define ST7789_SWRESET  0x01
#define ST7789_SLPOUT   0x11
#define ST7789_NORON    0x13
#define ST7789_INVOFF   0x20
#define ST7789_INVON    0x21
#define ST7789_DISPON   0x29
#define ST7789_CASET    0x2A
#define ST7789_RASET    0x2B
#define ST7789_RAMWR    0x2C
#define ST7789_MADCTL   0x36
#define ST7789_COLMOD   0x3A

#define DMA_MAX_COUNT   65535U

#define CS_LOW()    GPIO_ResetBits(LCD_GPIO_PORT, LCD_CS_GPIO_PIN)
#define CS_HIGH()   GPIO_SetBits(LCD_GPIO_PORT, LCD_CS_GPIO_PIN)
#define DC_CMD()    GPIO_ResetBits(LCD_GPIO_PORT, LCD_DC_GPIO_PIN)
#define DC_DATA()   GPIO_SetBits(LCD_GPIO_PORT, LCD_DC_GPIO_PIN)
#define RST_LOW()   GPIO_ResetBits(LCD_GPIO_PORT, LCD_RST_GPIO_PIN)
#define RST_HIGH()  GPIO_SetBits(LCD_GPIO_PORT, LCD_RST_GPIO_PIN)

// Source data for DMA fill (only this one half-word is read when the peripheral address does not increment)
static volatile uint16_t fill_color;

// ============================================================================
// SPI low level
// ============================================================================

static void spi_wait_idle(void)
{
    while (SPI_I2S_GetFlagStatus(LCD_SPI, SPI_I2S_FLAG_TXE) == RESET);
    while (SPI_I2S_GetFlagStatus(LCD_SPI, SPI_I2S_FLAG_BSY) == SET);
}

// SPE must be turned off before changing frame width; 16-bit frames send the high byte first, so RGB565 needs no byte swap
static void spi_set_16bit(bool en)
{
    spi_wait_idle();
    LCD_SPI->CTLR1 &= ~SPI_CTLR1_SPE;
    if (en)
        LCD_SPI->CTLR1 |= SPI_DataSize_16b;
    else
        LCD_SPI->CTLR1 &= ~SPI_DataSize_16b;
    LCD_SPI->CTLR1 |= SPI_CTLR1_SPE;
}

static void spi_write8(uint8_t data)
{
    while (SPI_I2S_GetFlagStatus(LCD_SPI, SPI_I2S_FLAG_TXE) == RESET);
    LCD_SPI->DATAR = data;
}

static void write_cmd(uint8_t cmd)
{
    spi_wait_idle();
    DC_CMD();
    spi_write8(cmd);
    spi_wait_idle();
    DC_DATA();
}

static void write_data(const uint8_t *data, uint8_t len)
{
    for (uint8_t i = 0; i < len; i++) spi_write8(data[i]);
}

static void write_cmd_data(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    CS_LOW();
    write_cmd(cmd);
    write_data(data, len);
    spi_wait_idle();
    CS_HIGH();
}

// ============================================================================
// DMA pixel transfer (16-bit frames, blocking)
// ============================================================================

static void dma_send16(const volatile uint16_t *src, uint32_t count, bool increment)
{
    spi_set_16bit(true);

    while (count > 0)
    {
        uint16_t n = (count > DMA_MAX_COUNT) ? DMA_MAX_COUNT : (uint16_t)count;

        DMA_Cmd(LCD_DMA_TX_CH, DISABLE);
        LCD_DMA_TX_CH->MADDR = (uint32_t)src;
        LCD_DMA_TX_CH->CNTR = n;
        if (increment)
            LCD_DMA_TX_CH->CFGR |= DMA_MemoryInc_Enable;
        else
            LCD_DMA_TX_CH->CFGR &= ~DMA_MemoryInc_Enable;
        DMA_ClearFlag(DMA1_FLAG_TC3);
        DMA_Cmd(LCD_DMA_TX_CH, ENABLE);

        while (DMA_GetFlagStatus(DMA1_FLAG_TC3) == RESET);
        DMA_ClearFlag(DMA1_FLAG_TC3);

        if (increment) src += n;
        count -= n;
    }

    DMA_Cmd(LCD_DMA_TX_CH, DISABLE);
    spi_set_16bit(false);
}

// ============================================================================
// Initialization
// ============================================================================

static void gpio_spi_init(void)
{
    RCC_APB2PeriphClockCmd(LCD_GPIO_CLK | LCD_SPI_CLK, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    GPIO_SetBits(LCD_GPIO_PORT, LCD_CS_GPIO_PIN | LCD_DC_GPIO_PIN | LCD_RST_GPIO_PIN);

    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = LCD_CS_GPIO_PIN | LCD_DC_GPIO_PIN | LCD_RST_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LCD_GPIO_PORT, &gpio);

    gpio.GPIO_Pin = LCD_SCK_GPIO_PIN | LCD_MOSI_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(LCD_GPIO_PORT, &gpio);

    // Single-line transmit only; mode 3 (CPOL=1, CPHA=1), compatible with modules with or without CS
    // SPI1 is on APB2 = 144MHz, /4 = 36MHz
    SPI_InitTypeDef spi = {0};
    spi.SPI_Direction = SPI_Direction_1Line_Tx;
    spi.SPI_Mode = SPI_Mode_Master;
    spi.SPI_DataSize = SPI_DataSize_8b;
    spi.SPI_CPOL = SPI_CPOL_High;
    spi.SPI_CPHA = SPI_CPHA_2Edge;
    spi.SPI_NSS = SPI_NSS_Soft;
    spi.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_4;
    spi.SPI_FirstBit = SPI_FirstBit_MSB;
    spi.SPI_CRCPolynomial = 7;
    SPI_Init(LCD_SPI, &spi);
    SPI_I2S_DMACmd(LCD_SPI, SPI_I2S_DMAReq_Tx, ENABLE);
    SPI_Cmd(LCD_SPI, ENABLE);

    DMA_InitTypeDef dma = {0};
    DMA_DeInit(LCD_DMA_TX_CH);
    dma.DMA_PeripheralBaseAddr = (uint32_t)&LCD_SPI->DATAR;
    dma.DMA_MemoryBaseAddr = (uint32_t)&fill_color;
    dma.DMA_DIR = DMA_DIR_PeripheralDST;
    dma.DMA_BufferSize = 1;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    dma.DMA_Mode = DMA_Mode_Normal;
    dma.DMA_Priority = DMA_Priority_High;
    dma.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(LCD_DMA_TX_CH, &dma);
}

void lcd_init(void)
{
    gpio_spi_init();

    RST_HIGH();
    delay_ms(5);
    RST_LOW();
    delay_ms(10);
    RST_HIGH();
    delay_ms(120);

    write_cmd_data(ST7789_SWRESET, NULL, 0);
    delay_ms(150);
    write_cmd_data(ST7789_SLPOUT, NULL, 0);
    delay_ms(120);

    write_cmd_data(ST7789_COLMOD, (const uint8_t[]){0x55}, 1);   // RGB565
    write_cmd_data(ST7789_MADCTL, (const uint8_t[]){LCD_MADCTL}, 1);
    write_cmd_data(LCD_INVERT ? ST7789_INVON : ST7789_INVOFF, NULL, 0);
    write_cmd_data(ST7789_NORON, NULL, 0);
    delay_ms(10);

    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, 0x0000);

    write_cmd_data(ST7789_DISPON, NULL, 0);
    delay_ms(20);
}

// ============================================================================
// Drawing
// ============================================================================

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    x0 += LCD_X_OFFSET; x1 += LCD_X_OFFSET;
    y0 += LCD_Y_OFFSET; y1 += LCD_Y_OFFSET;

    uint8_t caset[4] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
    uint8_t raset[4] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};

    CS_LOW();
    write_cmd(ST7789_CASET);
    write_data(caset, 4);
    write_cmd(ST7789_RASET);
    write_data(raset, 4);
    write_cmd(ST7789_RAMWR);
    // CS stays low; pulled high after the subsequent pixel transfer ends
}

static bool clip(uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h)
{
    if (*x >= LCD_WIDTH || *y >= LCD_HEIGHT || *w == 0 || *h == 0) return false;
    if (*x + *w > LCD_WIDTH) *w = LCD_WIDTH - *x;
    if (*y + *h > LCD_HEIGHT) *h = LCD_HEIGHT - *y;
    return true;
}

void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (!clip(&x, &y, &w, &h)) return;

    fill_color = color;
    lcd_set_window(x, y, x + w - 1, y + h - 1);
    dma_send16(&fill_color, (uint32_t)w * h, false);
    CS_HIGH();
}

// The caller guarantees pixels is a complete w*h rectangle; clipping of out-of-bounds parts is not supported
void lcd_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels)
{
    if (x + w > LCD_WIDTH || y + h > LCD_HEIGHT || w == 0 || h == 0) return;

    lcd_set_window(x, y, x + w - 1, y + h - 1);
    dma_send16(pixels, (uint32_t)w * h, true);
    CS_HIGH();
}
