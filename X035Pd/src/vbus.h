#pragma once

#include <stdint.h>

// VBUS voltage: PA0 divider (1/16) sampled by ADC, converted using the internal reference Vrefint (immune to VDD error)

void vbus_init(void);

// Called by the main loop every VBUS_SAMPLE_MS: sample and apply a first-order low-pass filter
void vbus_sample(void);

uint16_t vbus_mv(void);         // smoothed value
uint16_t vbus_mv_raw(void);     // most recent sample
