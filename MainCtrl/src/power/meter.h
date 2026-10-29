#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "board.h"
#include "drivers/ina2xx.h"

typedef struct meter_ch_t
{
    bool     online;            // INA2xx has been probed and the most recent read succeeded

    // Raw values: updated on every sample, used by the protection logic
    int32_t  vbus_mv;
    int32_t  current_ma;
    int32_t  power_mw;
    int32_t  temp_mc;

    // Smoothed values: first-order low-pass, used for display
    int32_t  vbus_mv_avg;
    int32_t  current_ma_avg;
    int32_t  power_mw_avg;

    // Accumulation (forward current only); ms is used as the time unit to avoid truncation at the 10ms step
    uint64_t energy_mwms;       // mW·ms, Wh = /3.6e9
    uint64_t charge_mams;       // mA·ms, Ah = /3.6e9
} meter_ch_t;

void meter_init(void);

// Called once every TASK_METER_PERIOD_MS
void meter_sample(uint32_t now_ms);

const meter_ch_t *meter_get(ch_id_t ch);

// Accumulated energy conversion: mW·ms -> mWh (without 64-bit division)
int32_t meter_mwms_to_mwh(uint64_t mwms);

// Set the accumulated values (restored from parameter storage at power-up, or cleared)
void meter_restore_energy(ch_id_t ch, uint64_t energy_mwms, uint64_t charge_mams);
const ina2xx_t *meter_get_sensor(ch_id_t ch);

// Sum of both channels (smoothed values)
int32_t meter_total_power_mw(void);
int32_t meter_total_current_ma(void);
