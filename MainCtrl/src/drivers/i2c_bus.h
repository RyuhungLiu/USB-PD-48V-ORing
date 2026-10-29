#pragma once

#include <stdbool.h>
#include <stdint.h>

// I2C1 master (PB6 SCL / PB7 SDA), 400kHz, polling + timeout
// All addresses are 7-bit; register addresses are 8-bit

void i2c_bus_init(void);

bool i2c_bus_write(uint8_t addr, uint8_t reg, const uint8_t *data, uint8_t len);
bool i2c_bus_read(uint8_t addr, uint8_t reg, uint8_t *data, uint8_t len);

// Accumulated error count (timeout / NACK), for diagnostics
uint32_t i2c_bus_error_count(void);
