#pragma once

// SDI virtual serial log (output over WCH-LinkE SWDIO, does not use USART1/2)
// In the release build with ENABLE_SDI_LOG=0 everything compiles to nothing

#if ENABLE_SDI_LOG

void log_init(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define LOG_INIT()      log_init()
#define LOG(fmt, ...)   log_printf(fmt "\r\n", ##__VA_ARGS__)

#else

#define LOG_INIT()      ((void)0)
#define LOG(fmt, ...)   ((void)0)

#endif
