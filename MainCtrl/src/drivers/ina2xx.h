#pragma once

#include <stdbool.h>
#include <stdint.h>

// TI 85V power monitors (pin compatible), identified automatically by DEVICE_ID:
//   INA237 (SBOSA20) / INA238 16-bit: VBUS 3.125mV/LSB, VSHUNT 5uV/LSB, DIETEMP[15:4] 125m degC/LSB
//   INA228 (SBOS938) 20-bit: VBUS 195.3125uV/LSB, VSHUNT 312.5nV/LSB, DIETEMP 7.8125m degC/LSB
// The schematic specifies INA237; the first assembled batch was marked "237I" but read ID 0x2281 and the ADC did not convert, judged to be fake chips
//
// All use ADCRANGE=0 (shunt full scale +/-163.84mV, +/-16.384A at 10mOhm)
//   INA237/238: CURRENT_LSB = 16.384A/2^15 = 500uA,   SHUNT_CAL = 819.2e6   × I_LSB × R = 4096
//   INA228:     CURRENT_LSB = 16.384A/2^19 = 31.25uA, SHUNT_CAL = 13107.2e6 × I_LSB × R = 4096

#define INA2XX_MANUFACTURER_ID  0x5449  // "TI"

typedef enum ina2xx_chip_t
{
    INA_CHIP_UNKNOWN = 0,
    INA_CHIP_237,
    INA_CHIP_238,
    INA_CHIP_228,
} ina2xx_chip_t;

typedef enum ina2xx_fault_t
{
    INA_FAULT_NONE = 0,
    INA_FAULT_NO_ACK,           // no I2C acknowledge
    INA_FAULT_BAD_MFG,          // manufacturer ID is not TI
    INA_FAULT_UNSUPPORTED,      // device ID not in the supported list
    INA_FAULT_CONFIG,           // configuration write or read-back failed
    INA_FAULT_TRIM,             // MEMSTAT=0 and ADC not converting
    INA_FAULT_NO_CONVERSION,    // ADC not converting
} ina2xx_fault_t;

typedef struct ina2xx_t
{
    uint8_t  addr;
    bool     present;           // initialized successfully and ADC confirmed converting
    ina2xx_chip_t chip;
    ina2xx_fault_t fault;
    uint16_t manufacturer_id;
    uint16_t device_id;
    uint16_t shunt_cal;
    uint32_t current_lsb_na;    // nA per LSB
    bool     trim_ok;           // DIAG_ALRT.MEMSTAT: on-chip trim memory checksum OK

    // Latest reading
    int32_t  vbus_mv;
    int32_t  vshunt_uv;
    int32_t  current_ua;
    int32_t  temp_mc;           // m°C

    uint32_t read_errors;
} ina2xx_t;

// Probe ID, reset and configure: continuous VBUS/VSHUNT/TEMP measurement, 1052/1052/540us, 4-sample averaging (~10.6ms per round)
// Finally waits for one conversion to complete, blocking at most about 50ms
bool ina2xx_init(ina2xx_t *dev, uint8_t addr, uint32_t shunt_uohm);

// Read VBUS / VSHUNT / CURRENT / DIETEMP; on failure keep the previous values and return false
bool ina2xx_read(ina2xx_t *dev);

const char *ina2xx_chip_name(ina2xx_chip_t chip);
const char *ina2xx_fault_str(ina2xx_fault_t fault);
