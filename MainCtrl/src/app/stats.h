#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "board.h"

// Run statistics (docs/FLOW.md section 8)
//   Session statistics (RAM, restart accumulating at power-on or after user clear): per-channel on time, peak current/power, max temperature,
//     trip counts by type, last trip, combined peak power of both channels, current sharing run time
//   Accumulated statistics (saved with the parameter record): boot count, total output on time, total trip count; accumulated Wh/Ah see meter

// Trip reason categories, matching the trip reason strings in power/oring.c
typedef enum stats_trip_kind_t
{
    TRIP_OCP = 0,
    TRIP_SCP,
    TRIP_REVERSE,
    TRIP_OTP,
    TRIP_OVP,
    TRIP_VOLTAGE,
    TRIP_DETACH,
    TRIP_HARD_RESET,
    TRIP_LINK_LOST,
    TRIP_ISENSE_LOST,
    TRIP_CONTRACT,
    TRIP_VSET,
    TRIP_OTHER,
    TRIP_KIND_COUNT,
} stats_trip_kind_t;

typedef struct stats_ch_t
{
    uint32_t on_ms;                     // cumulative output on time
    int32_t  peak_ma;
    int32_t  peak_mw;
    int32_t  max_temp_mc;
    uint16_t trips_latched;
    uint16_t trips_auto;                // non-latched (can auto-recover)
    uint16_t trip_count[TRIP_KIND_COUNT];

    // Voltage sag record (while conducting with a contract, raw 10ms samples)
    uint16_t min_mv;                    // lowest VBUS, 0 = no record
    int16_t  min_ma;                    // current at the lowest voltage
    uint16_t min_contract_mv;           // contract voltage at the lowest voltage
    uint32_t min_ms;                    // time of occurrence (run time)
    uint16_t sag_count;                 // number of times below the contract voltage by STATS_SAG_PCT%
    uint16_t sag_max_ms;                // longest single duration
    uint32_t sag_since;                 // start of the current sag, 0 = not sagging
} stats_ch_t;

typedef struct stats_t
{
    uint32_t   since_ms;                // statistics start (power-on or clear time)
    stats_ch_t ch[CH_COUNT];
    int32_t    peak_total_mw;
    uint32_t   share_run_ms;

    // Last trip (either channel)
    bool        last_valid;
    ch_id_t     last_ch;
    const char *last_reason;
    int32_t     last_value;
    const char *last_unit;
    bool        last_latched;
    uint32_t    last_ms;
} stats_t;

// Call after app_init (parameters read)
void stats_init(void);

// Call every TASK_METER_PERIOD_MS after meter / oring / share
void stats_sample(uint32_t now_ms);

// Called by power/oring.c on every automatic trip
void stats_on_trip(ch_id_t ch, const char *reason, bool latched, int32_t value, const char *unit, uint32_t now_ms);

// Clear session statistics (accumulated statistics unchanged)
void stats_reset_session(uint32_t now_ms);

const stats_t *stats_get(void);
const char *stats_trip_kind_str(stats_trip_kind_t kind);

// Accumulated statistics (including this session), saved by parameter storage
uint32_t stats_life_boots(void);
uint32_t stats_life_on_s(void);         // total time any channel output was on
uint32_t stats_life_trips(void);
