// Minimal snprintf / vsnprintf: replaces the newlib-nano printf family (together with the malloc / reent / impure it pulls in, saves about 3KB)
// Supports: %d %i %u %x %X %c %s %%; flags - 0 + space; width (number or *); precision .N / .* (%s truncation, minimum digits for integers);
//       length modifiers l / h / hh / z (int, long, size_t are all 32-bit; h / hh truncate per the standard)
// No floating point, no %p %o %n. Return value as in the standard: number of characters that would be written given enough buffer (excluding the terminating 0)
// Same names as the library functions: take precedence over the versions in libc_nano.a at link time; used prevents LTO from dropping the definitions after builtin folding

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifndef TINY_PRINTF_NAME
#define TINY_PRINTF_NAME(n) n
#endif

typedef struct out_t
{
    char  *buf;
    size_t size;
    size_t len;
} out_t;

static void put(out_t *o, char c)
{
    if (o->len + 1 < o->size) o->buf[o->len] = c;
    o->len++;
}

static void pad(out_t *o, char c, int n)
{
    while (n-- > 0) put(o, c);
}

__attribute__((used)) int TINY_PRINTF_NAME(vsnprintf)(char *buf, size_t size, const char *fmt, va_list ap)
{
    out_t o = {buf, size, 0};

    for (; *fmt; fmt++)
    {
        if (*fmt != '%')
        {
            put(&o, *fmt);
            continue;
        }

        bool left = false, zero = false;
        char sign = 0;
        for (;;)
        {
            char c = *++fmt;
            if (c == '-') left = true;
            else if (c == '0') zero = true;
            else if (c == '+') sign = '+';
            else if (c == ' ') { if (!sign) sign = ' '; }
            else break;
        }

        int width = 0;
        if (*fmt == '*')
        {
            width = va_arg(ap, int);
            if (width < 0) { left = true; width = -width; }
            fmt++;
        }
        else
        {
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }

        int prec = -1;
        if (*fmt == '.')
        {
            fmt++;
            prec = 0;
            if (*fmt == '*')
            {
                prec = va_arg(ap, int);
                fmt++;
            }
            else
            {
                while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
            }
        }

        int8_t hlen = 0;                    // 1 = h, 2 = hh
        while (*fmt == 'l' || *fmt == 'z') fmt++;
        while (*fmt == 'h') { hlen++; fmt++; }

        char tmp[11];
        const char *s = tmp;
        int len = 0;
        char pre = 0;
        bool num = true;
        uint32_t u;
        unsigned base = 10;
        char conv = *fmt;

        switch (conv)
        {
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            len = 1;
            num = false;
            break;
        case 's':
            s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (s[len] && (prec < 0 || len < prec)) len++;
            num = false;
            break;
        case 'd':
        case 'i':
        {
            int32_t v = va_arg(ap, int32_t);
            if (hlen == 1) v = (int16_t)v;
            else if (hlen >= 2) v = (int8_t)v;
            u = (v < 0) ? 0u - (uint32_t)v : (uint32_t)v;
            pre = (v < 0) ? '-' : sign;
            goto digits;
        }
        case 'x':
        case 'X':
            base = 16;
            /* fall through */
        case 'u':
            u = va_arg(ap, uint32_t);
            if (hlen == 1) u = (uint16_t)u;
            else if (hlen >= 2) u = (uint8_t)u;
        digits:
            // Write from the lowest digit upward into the tail of tmp; with precision 0 and value 0 no digits are output (as in the standard)
            len = 0;
            while (u || (len == 0 && prec != 0))
            {
                unsigned d = u % base;
                tmp[sizeof(tmp) - 1 - len++] = (char)(d < 10 ? '0' + d : (conv == 'X' ? 'A' : 'a') + d - 10);
                u /= base;
            }
            s = tmp + sizeof(tmp) - len;
            break;
        case '\0':
            fmt--;                          // format string ends with '%': ignore
            continue;
        default:                            // '%%' and unsupported conversions: output as is
            put(&o, conv);
            continue;
        }

        // Integers: precision gives the minimum number of digits; the 0 flag is ignored in that case
        int zeros = (num && prec > len) ? prec - len : 0;
        if (num && prec >= 0) zero = false;
        int fill = width - len - zeros - (pre ? 1 : 0);

        if (!left && !(zero && num)) pad(&o, ' ', fill);
        if (pre) put(&o, pre);
        if (!left && zero && num) pad(&o, '0', fill);
        pad(&o, '0', zeros);
        for (int i = 0; i < len; i++) put(&o, s[i]);
        if (left) pad(&o, ' ', fill);
    }

    if (size) buf[(o.len < size) ? o.len : size - 1] = '\0';
    return (int)o.len;
}

__attribute__((used)) int TINY_PRINTF_NAME(snprintf)(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = TINY_PRINTF_NAME(vsnprintf)(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
