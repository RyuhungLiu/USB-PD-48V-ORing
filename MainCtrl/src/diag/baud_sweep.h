#pragma once

#include <stdint.h>

#ifndef DIAG_BAUD_SWEEP
#define DIAG_BAUD_SWEEP 0
#endif

#if DIAG_BAUD_SWEEP
void baud_sweep_init(uint32_t now_ms);
void baud_sweep_process(uint32_t now_ms);
#endif
