#pragma once

#include <stdbool.h>
#include <stdint.h>

// Power trend record: samples both channels' smoothed power every TREND_FINE_MS
//   Fine scale: 100ms / point, TREND_LEN points (30 seconds)
//   Coarse scale: average of 10 fine points (1s) / point, TREND_LEN points (5 minutes)
// Always recorded regardless of the current page; unit 0.1W

#define TREND_LEN           300
#define TREND_FINE_MS       100
#define TREND_COARSE_DIV    10

typedef enum trend_scale_t
{
    TREND_FINE = 0,
    TREND_COARSE,
    TREND_SCALE_COUNT,
} trend_scale_t;

typedef struct trend_pt_t
{
    int16_t p1;     // CH1 power, 0.1W
    int16_t p2;     // CH2 power, 0.1W
} trend_pt_t;

// Call every TASK_METER_PERIOD_MS (after meter_sample); internally samples at TREND_FINE_MS
void trend_sample(uint32_t now_ms);

// Number of points available (<= TREND_LEN)
uint16_t trend_count(trend_scale_t scale);

// The i-th point (0 = oldest)
trend_pt_t trend_get(trend_scale_t scale, uint16_t i);

// +1 for every point written, used to decide whether a redraw is needed
uint32_t trend_seq(trend_scale_t scale);

// Milliseconds represented by each point
uint16_t trend_period_ms(trend_scale_t scale);
