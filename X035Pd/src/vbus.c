#include "vbus.h"

#include "board.h"

static uint16_t raw_mv;
static uint16_t avg_mv;
static uint16_t vref_raw = 1489;    // estimate assuming VDD = 3.3V before the first sample (1200mV * 4095 / 3300)

static uint16_t adc_read(uint8_t channel)
{
    ADC_RegularChannelConfig(ADC1, channel, 1, ADC_SampleTime_11Cycles);
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);
    while (!ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC)) {}
    return ADC_GetConversionValue(ADC1);
}

void vbus_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    ADC_InitTypeDef adc = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_ADC1, ENABLE);
    gpio.GPIO_Pin = BOARD_VBUS_ADC_PIN;
    gpio.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(BOARD_VBUS_ADC_PORT, &gpio);

    ADC_CLKConfig(ADC1, ADC_CLK_Div6);
    adc.ADC_Mode = ADC_Mode_Independent;
    adc.ADC_ScanConvMode = DISABLE;
    adc.ADC_ContinuousConvMode = DISABLE;
    adc.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    adc.ADC_DataAlign = ADC_DataAlign_Right;
    adc.ADC_NbrOfChannel = 1;
    ADC_Init(ADC1, &adc);
    ADC_Cmd(ADC1, ENABLE);

    vbus_sample();
    avg_mv = raw_mv;
}

void vbus_sample(void)
{
    uint16_t v = adc_read(ADC_Channel_Vrefint);
    if (v > 100) vref_raw = (uint16_t)((vref_raw * 7u + v) / 8u);
    uint32_t a = adc_read(BOARD_VBUS_ADC_CHANNEL);
    raw_mv = (uint16_t)(a * BOARD_VREFINT_MV * BOARD_VBUS_DIV_NUM / vref_raw);
    // First-order low-pass (time constant about 4 samples); follows directly on large voltage changes, making plug/unplug easy to detect
    int32_t d = (int32_t)raw_mv - avg_mv;
    if (d > 2000 || d < -2000) avg_mv = raw_mv;
    else avg_mv = (uint16_t)(avg_mv + d / 4);
}

uint16_t vbus_mv(void)
{
    return avg_mv;
}

uint16_t vbus_mv_raw(void)
{
    return raw_mv;
}
