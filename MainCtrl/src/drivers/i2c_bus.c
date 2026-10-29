#include "i2c_bus.h"

#include "board.h"
#include "bsp/delay.h"

#define I2C_SPEED_HZ        400000
#define I2C_TIMEOUT_US      2000

static uint32_t error_count = 0;

// ============================================================================
// Initialization and bus recovery
// ============================================================================

static void periph_init(void)
{
    RCC_APB1PeriphResetCmd(INA_I2C_CLK, ENABLE);
    RCC_APB1PeriphResetCmd(INA_I2C_CLK, DISABLE);

    I2C_InitTypeDef i2c = {0};
    i2c.I2C_ClockSpeed = I2C_SPEED_HZ;
    i2c.I2C_Mode = I2C_Mode_I2C;
    i2c.I2C_DutyCycle = I2C_DutyCycle_2;
    i2c.I2C_OwnAddress1 = 0x00;
    i2c.I2C_Ack = I2C_Ack_Enable;
    i2c.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    I2C_Init(INA_I2C, &i2c);
    I2C_Cmd(INA_I2C, ENABLE);
}

static void pins_af(void)
{
    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = INA_SCL_GPIO_PIN | INA_SDA_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_AF_OD;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(INA_GPIO_PORT, &gpio);
}

// When a slave is stuck holding SDA low: clock out 9 extra SCL pulses in GPIO mode, then send a STOP
static void bus_recover(void)
{
    I2C_Cmd(INA_I2C, DISABLE);

    GPIO_SetBits(INA_GPIO_PORT, INA_SCL_GPIO_PIN | INA_SDA_GPIO_PIN);
    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = INA_SCL_GPIO_PIN | INA_SDA_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Out_OD;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(INA_GPIO_PORT, &gpio);

    for (uint8_t i = 0; i < 9; i++)
    {
        if (GPIO_ReadInputDataBit(INA_GPIO_PORT, INA_SDA_GPIO_PIN)) break;
        GPIO_ResetBits(INA_GPIO_PORT, INA_SCL_GPIO_PIN);
        delay_us(5);
        GPIO_SetBits(INA_GPIO_PORT, INA_SCL_GPIO_PIN);
        delay_us(5);
    }

    // STOP: SDA goes from low to high while SCL is high
    GPIO_ResetBits(INA_GPIO_PORT, INA_SDA_GPIO_PIN);
    delay_us(5);
    GPIO_SetBits(INA_GPIO_PORT, INA_SCL_GPIO_PIN);
    delay_us(5);
    GPIO_SetBits(INA_GPIO_PORT, INA_SDA_GPIO_PIN);
    delay_us(5);

    pins_af();
    periph_init();
}

void i2c_bus_init(void)
{
    RCC_APB2PeriphClockCmd(INA_GPIO_CLK, ENABLE);
    RCC_APB1PeriphClockCmd(INA_I2C_CLK, ENABLE);

    bus_recover();
}

// ============================================================================
// Transfer
// ============================================================================

static bool wait_event(uint32_t event)
{
    uint32_t start = micros();
    while (I2C_CheckEvent(INA_I2C, event) != READY)
    {
        if ((uint32_t)(micros() - start) > I2C_TIMEOUT_US) return false;
    }
    return true;
}

static bool wait_flag(uint32_t flag, FlagStatus state)
{
    uint32_t start = micros();
    while (I2C_GetFlagStatus(INA_I2C, flag) != state)
    {
        if ((uint32_t)(micros() - start) > I2C_TIMEOUT_US) return false;
    }
    return true;
}

static bool fail(void)
{
    error_count++;
    I2C_GenerateSTOP(INA_I2C, ENABLE);
    I2C_AcknowledgeConfig(INA_I2C, ENABLE);
    bus_recover();
    return false;
}

// START + address (write) + register address
static bool start_with_reg(uint8_t addr, uint8_t reg)
{
    if (!wait_flag(I2C_FLAG_BUSY, RESET)) return false;

    I2C_GenerateSTART(INA_I2C, ENABLE);
    if (!wait_event(I2C_EVENT_MASTER_MODE_SELECT)) return false;

    I2C_Send7bitAddress(INA_I2C, addr << 1, I2C_Direction_Transmitter);
    if (!wait_event(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED)) return false;

    I2C_SendData(INA_I2C, reg);
    if (!wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED)) return false;

    return true;
}

bool i2c_bus_write(uint8_t addr, uint8_t reg, const uint8_t *data, uint8_t len)
{
    if (!start_with_reg(addr, reg)) return fail();

    for (uint8_t i = 0; i < len; i++)
    {
        I2C_SendData(INA_I2C, data[i]);
        if (!wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED)) return fail();
    }

    I2C_GenerateSTOP(INA_I2C, ENABLE);
    return true;
}

// Disable ACK and preset STOP before the last byte; must not be interrupted between two RXNE for more than one byte time (~22us)
bool i2c_bus_read(uint8_t addr, uint8_t reg, uint8_t *data, uint8_t len)
{
    if (len == 0) return true;
    if (!start_with_reg(addr, reg)) return fail();

    I2C_GenerateSTART(INA_I2C, ENABLE);
    if (!wait_event(I2C_EVENT_MASTER_MODE_SELECT)) return fail();

    I2C_Send7bitAddress(INA_I2C, addr << 1, I2C_Direction_Receiver);
    if (!wait_event(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED)) return fail();

    for (uint8_t i = 0; i < len; i++)
    {
        if (i == len - 1)
        {
            I2C_AcknowledgeConfig(INA_I2C, DISABLE);
            I2C_GenerateSTOP(INA_I2C, ENABLE);
        }
        if (!wait_flag(I2C_FLAG_RXNE, SET)) return fail();
        data[i] = I2C_ReceiveData(INA_I2C);
    }

    I2C_AcknowledgeConfig(INA_I2C, ENABLE);
    return true;
}

uint32_t i2c_bus_error_count(void)
{
    return error_count;
}
