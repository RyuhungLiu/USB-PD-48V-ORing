#include "log.h"

#if ENABLE_SDI_LOG

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include "delay.h"

// SDI data register: low byte of DATA0 is the length of this chunk (<=7), cleared after the debugger takes it
#define SDI_DATA0 ((volatile uint32_t *)0xE0000380)
#define SDI_DATA1 ((volatile uint32_t *)0xE0000384)

// DATA0 is not cleared when the debugger is not polling; on wait timeout this entry is dropped and the next one retries (debugger can attach anytime)
// Without a debugger each log entry blocks at most SDI_WAIT_TIMEOUT_US
#define SDI_WAIT_TIMEOUT_US 2000

static bool sdi_wait_ready(void)
{
    uint32_t start = micros();
    while (*SDI_DATA0 != 0u)
    {
        if ((uint32_t)(micros() - start) > SDI_WAIT_TIMEOUT_US) return false;
    }
    return true;
}

static void sdi_write(const char *buf, int len)
{
    int i = 0;
    while (len > 0)
    {
        if (!sdi_wait_ready()) return;

        uint8_t n = (len > 7) ? 7 : (uint8_t)len;
        uint8_t b[7] = {0};
        for (uint8_t k = 0; k < n; k++) b[k] = (uint8_t)buf[i + k];

        *SDI_DATA1 = b[3] | (b[4] << 8) | (b[5] << 16) | ((uint32_t)b[6] << 24);
        *SDI_DATA0 = n | (b[0] << 8) | (b[1] << 16) | ((uint32_t)b[2] << 24);

        i += n;
        len -= n;
    }
}

void log_init(void)
{
    *SDI_DATA0 = 0;
}

void log_printf(const char *fmt, ...)
{
    static char buf[128];

    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len <= 0) return;
    if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;

    sdi_write(buf, len);
}

#endif
