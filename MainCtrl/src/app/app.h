#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "board.h"

// Power-on policy (docs/FLOW.md section 8)
//   - Read parameter storage, restore accumulated energy
//   - When the X035 link first comes online, if its trigger hold config differs from the saved target voltage, push it again (main controller wins, one channel at a time)
//   - When "auto output on power-up" is enabled, both channels wait using the target voltage as reference and turn on automatically once the contract matches

void app_init(void);                // call after meter_init, pd_node_init, oring_init
void app_process(uint32_t now_ms);  // call every 10ms

// Switch voltage: turn off the corresponding output, save the target voltage (written to flash immediately), then send one channel at a time, avoiding a simultaneous
// Hard Reset on both channels that would power down the whole board. Returns the bitmap of channels actually accepted (bit0 = CH1); channels whose link is not ready are not accepted
uint8_t app_set_voltage(uint8_t ch_mask, uint16_t mv);

// Some channel is still queued or switching
bool app_vset_busy(void);

// This channel is waiting in the queue for the previous channel to finish switching
bool app_vset_queued(ch_id_t ch);
