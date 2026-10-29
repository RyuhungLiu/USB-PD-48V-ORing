#pragma once

#include <stddef.h>
#include <stdint.h>

// Fixed-point formatting: value is in units of 10^-scale (e.g. scale=3 for mV), keeps decimals digits after the point with rounding
// Example: fmt_fixed(buf, n, 48123, 3, 2) -> "48.12"; fmt_fixed(buf, n, -12, 3, 3) -> "-0.012"
// Returns the number of characters written
int fmt_fixed(char *buf, size_t size, int32_t value, uint8_t scale, uint8_t decimals);
