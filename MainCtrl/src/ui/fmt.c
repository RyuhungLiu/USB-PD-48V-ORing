#include "fmt.h"

#include <stdbool.h>
#include <stdio.h>

static uint32_t pow10u(uint8_t n)
{
    uint32_t p = 1;
    while (n--) p *= 10;
    return p;
}

int fmt_fixed(char *buf, size_t size, int32_t value, uint8_t scale, uint8_t decimals)
{
    if (decimals > scale) decimals = scale;

    bool neg = value < 0;
    uint32_t mag = neg ? (uint32_t)(-(int64_t)value) : (uint32_t)value;

    uint32_t drop = pow10u(scale - decimals);
    mag = (mag + drop / 2) / drop;

    uint32_t div = pow10u(decimals);
    uint32_t ip = mag / div;
    uint32_t fp = mag % div;

    // Do not show the minus sign when the rounded value is 0
    if (mag == 0) neg = false;

    if (decimals == 0)
        return snprintf(buf, size, "%s%lu", neg ? "-" : "", ip);

    return snprintf(buf, size, "%s%lu.%0*lu", neg ? "-" : "", ip, decimals, fp);
}
