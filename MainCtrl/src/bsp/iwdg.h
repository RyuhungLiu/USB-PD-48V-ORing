#pragma once

#include <stdint.h>

void iwdg_init(uint16_t timeout_ms);
void iwdg_feed(void);
