#include "ina2xx.h"

#include "drivers/i2c_bus.h"
#include "bsp/delay.h"

#define REG_CONFIG          0x00
#define REG_ADC_CONFIG      0x01
#define REG_SHUNT_CAL       0x02
#define REG_VSHUNT          0x04
#define REG_VBUS            0x05
#define REG_DIETEMP         0x06
#define REG_CURRENT         0x07
#define REG_DIAG_ALRT       0x0B
#define REG_MANUFACTURER_ID 0x3E
#define REG_DEVICE_ID       0x3F

#define DIE_ID_INA237       0x237
#define DIE_ID_INA228       0x228
#define DIE_ID_INA238       0x238

#define CONFIG_RST          (1U << 15)
#define DIAG_MEMSTAT        (1U << 0)
#define DIAG_CNVRF          (1U << 1)

// ADC_CONFIG fields (same layout on both chips)
#define ADC_MODE_CONT_ALL   0xF         // continuous VBUS + VSHUNT + TEMP
#define ADC_CT_540US        4
#define ADC_CT_1052US       5
#define ADC_AVG_4           1

#define ADC_CONFIG_VALUE    ((ADC_MODE_CONT_ALL << 12) | (ADC_CT_1052US << 9) | \
                             (ADC_CT_1052US << 6) | (ADC_CT_540US << 3) | ADC_AVG_4)

// ============================================================================
// Register read/write (big endian)
// ============================================================================

static bool read_u16(const ina2xx_t *dev, uint8_t reg, uint16_t *val)
{
    uint8_t b[2];
    if (!i2c_bus_read(dev->addr, reg, b, 2)) return false;
    *val = ((uint16_t)b[0] << 8) | b[1];
    return true;
}

// INA228 24-bit register, data in [23:4], returns signed 20 bits
static bool read_s20(const ina2xx_t *dev, uint8_t reg, int32_t *val)
{
    uint8_t b[3];
    if (!i2c_bus_read(dev->addr, reg, b, 3)) return false;
    *val = (int32_t)(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8)) >> 12;
    return true;
}

static bool write_u16(const ina2xx_t *dev, uint8_t reg, uint16_t val)
{
    uint8_t b[2] = {val >> 8, val & 0xFF};
    return i2c_bus_write(dev->addr, reg, b, 2);
}

// ============================================================================
// Initialization
// ============================================================================

// One conversion round takes about 10.6ms; leave ample margin
#define CONVERSION_WAIT_MS  50

static bool fault(ina2xx_t *dev, ina2xx_fault_t f)
{
    dev->fault = f;
    return false;
}

// Wait for one conversion to complete (DIAG_ALRT.CNVRF); reading DIAG_ALRT clears that flag
static bool wait_conversion(const ina2xx_t *dev)
{
    uint32_t start = millis();
    do
    {
        uint16_t diag = 0;
        if (read_u16(dev, REG_DIAG_ALRT, &diag) && (diag & DIAG_CNVRF)) return true;
        delay_ms(2);
    } while ((uint32_t)(millis() - start) < CONVERSION_WAIT_MS);
    return false;
}

bool ina2xx_init(ina2xx_t *dev, uint8_t addr, uint32_t shunt_uohm)
{
    *dev = (ina2xx_t){0};
    dev->addr = addr;

    if (!read_u16(dev, REG_MANUFACTURER_ID, &dev->manufacturer_id)) return fault(dev, INA_FAULT_NO_ACK);
    if (dev->manufacturer_id != INA2XX_MANUFACTURER_ID) return fault(dev, INA_FAULT_BAD_MFG);
    if (!read_u16(dev, REG_DEVICE_ID, &dev->device_id)) return fault(dev, INA_FAULT_NO_ACK);

    // SHUNT_CAL = k x I_LSB[A] x R[Ohm]; expressed in nA and uOhm = k' x I_LSB[nA] x R[uOhm] / 1e10
    // INA238 and INA237 have identical register definitions and scaling, only higher precision
    uint32_t cal_k;
    switch (dev->device_id >> 4)
    {
        case DIE_ID_INA237:
        case DIE_ID_INA238:
            dev->chip = ((dev->device_id >> 4) == DIE_ID_INA237) ? INA_CHIP_237 : INA_CHIP_238;
            dev->current_lsb_na = 500000;   // 500uA
            cal_k = 8192;                   // 819.2e6 / 1e5
            break;
        case DIE_ID_INA228:
            dev->chip = INA_CHIP_228;
            dev->current_lsb_na = 31250;    // 31.25uA
            cal_k = 131072;                 // 13107.2e6 / 1e5
            break;
        default:
            return fault(dev, INA_FAULT_UNSUPPORTED);
    }

    if (!write_u16(dev, REG_CONFIG, CONFIG_RST)) return fault(dev, INA_FAULT_CONFIG);
    delay_ms(1);

    // DIAG_ALRT.MEMSTAT (bit0): should be 1 after reset; 0 means an on-chip trim memory checksum error
    uint16_t diag = 0;
    if (read_u16(dev, REG_DIAG_ALRT, &diag)) dev->trim_ok = (diag & DIAG_MEMSTAT) != 0;

    // SHUNT_CAL = cal_k x I_LSB(nA) x R(uOhm) / 1e10; each term is first reduced by 8 / 125 / 1000, all in 32 bits
    // (cal_k is a multiple of 8, I_LSB a multiple of 125nA, R a multiple of 1mOhm; at 10mOhm both chips give 4096)
    uint32_t cal = ((cal_k / 8) * (dev->current_lsb_na / 125) * (shunt_uohm / 1000) + 5000) / 10000;
    if (cal > 0x7FFF) cal = 0x7FFF;
    dev->shunt_cal = (uint16_t)cal;

    if (!write_u16(dev, REG_CONFIG, 0) ||               // ADCRANGE=0, no conversion delay, no accumulation
        !write_u16(dev, REG_SHUNT_CAL, dev->shunt_cal) ||
        !write_u16(dev, REG_ADC_CONFIG, ADC_CONFIG_VALUE))
        return fault(dev, INA_FAULT_CONFIG);

    // Read back and verify, in case the write was disturbed
    uint16_t rb = 0;
    if (!read_u16(dev, REG_SHUNT_CAL, &rb) || rb != dev->shunt_cal) return fault(dev, INA_FAULT_CONFIG);
    if (!read_u16(dev, REG_ADC_CONFIG, &rb) || rb != ADC_CONFIG_VALUE) return fault(dev, INA_FAULT_CONFIG);

    // A device that answers I2C but whose ADC does not convert (common with refurbished/fake chips) must not be used as 0V/0A
    if (!wait_conversion(dev)) return fault(dev, dev->trim_ok ? INA_FAULT_NO_CONVERSION : INA_FAULT_TRIM);

    dev->fault = INA_FAULT_NONE;
    dev->present = true;
    return true;
}

// ============================================================================
// Read
// ============================================================================

static bool read_ina237(ina2xx_t *dev)
{
    uint16_t vbus, vshunt, current, temp;
    if (!read_u16(dev, REG_VBUS, &vbus) ||
        !read_u16(dev, REG_VSHUNT, &vshunt) ||
        !read_u16(dev, REG_CURRENT, &current) ||
        !read_u16(dev, REG_DIETEMP, &temp))
        return false;

    dev->vbus_mv = (int32_t)(((uint32_t)vbus * 25 + 4) / 8);         // 3.125mV = 25/8
    dev->vshunt_uv = (int32_t)(int16_t)vshunt * 5;
    dev->current_ua = (int32_t)(int16_t)current * (int32_t)(dev->current_lsb_na / 1000);
    dev->temp_mc = ((int32_t)(int16_t)temp >> 4) * 125;
    return true;
}

static bool read_ina228(ina2xx_t *dev)
{
    int32_t vbus, vshunt, current;
    uint16_t temp;
    if (!read_s20(dev, REG_VBUS, &vbus) ||
        !read_s20(dev, REG_VSHUNT, &vshunt) ||
        !read_s20(dev, REG_CURRENT, &current) ||
        !read_u16(dev, REG_DIETEMP, &temp))
        return false;

    if (vbus < 0) vbus = 0;
    // 20-bit raw value; the products below all fit in 32 bits
    dev->vbus_mv = (vbus * 3125 + 8000) / 16000;                       // 195.3125uV = 3125/16 uV
    dev->vshunt_uv = (vshunt * 5) / 16;                                 // 312.5nV = 5/16 uV
    dev->current_ua = current * (int32_t)(dev->current_lsb_na / 125) / 8;   // nA/1000 = (nA/125)/8
    dev->temp_mc = ((int32_t)(int16_t)temp * 125) / 16;                // 7.8125m°C = 125/16
    return true;
}

bool ina2xx_read(ina2xx_t *dev)
{
    if (!dev->present) return false;

    bool ok = (dev->chip == INA_CHIP_228) ? read_ina228(dev) : read_ina237(dev);
    if (!ok) dev->read_errors++;
    return ok;
}

const char *ina2xx_chip_name(ina2xx_chip_t chip)
{
    switch (chip)
    {
        case INA_CHIP_237: return "INA237";
        case INA_CHIP_238: return "INA238";
        case INA_CHIP_228: return "INA228";
        default:           return "INA2xx";
    }
}

const char *ina2xx_fault_str(ina2xx_fault_t fault)
{
    switch (fault)
    {
        case INA_FAULT_NONE:          return "OK";
        case INA_FAULT_NO_ACK:        return "NO ACK";
        case INA_FAULT_BAD_MFG:       return "BAD MFG ID";
        case INA_FAULT_UNSUPPORTED:   return "UNSUPPORTED";
        case INA_FAULT_CONFIG:        return "CONFIG FAIL";
        case INA_FAULT_TRIM:          return "NO ADC/TRIM";
        case INA_FAULT_NO_CONVERSION: return "NO ADC";
        default:                      return "?";
    }
}
