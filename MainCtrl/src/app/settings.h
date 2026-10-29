#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "board.h"

// Parameter storage (docs/FLOW.md section 8)
//   The last 8KB of flash is split into two 4KB sectors that take turns appending records (64B each, magic + sequence + CRC32),
//   at power-on the valid record with the highest sequence is used. The other sector is erased only after one fills up, so the old record survives a power loss during erase.
//   After a settings change, write is delayed by SETTINGS_SAVE_DELAY_MS; accumulated energy is written every SETTINGS_ENERGY_PERIOD_MS

// Learned result of MANUAL current sharing feedforward: offset difference between channels d_off (CH1 - CH2, mV) = a_mv + k_mv_per_a x total current (A)
typedef struct share_profile_t
{
    uint32_t fp[CH_COUNT];              // fingerprint of both channels' capability tables at learning time (not applied if a charger was swapped)
    uint16_t v0;                        // center voltage at learning time
    int16_t  a_mv;
    int16_t  k_mv_per_a;
    uint16_t resid_mv;                  // maximum fit residual
    uint8_t  bins;                      // number of current bins used in the fit
    uint8_t  valid;
    uint8_t  pad[2];
} share_profile_t;

typedef struct settings_t
{
    bool     sync_on;                   // voltage page SYNC: switch both channels together
    bool     auto_on;                   // after power-on, turn on output automatically when the contract matches the target voltage
    bool     balance_on;                // current sharing switch (when on, current sharing is entered automatically once conditions are met)
    uint8_t  balance_mode;              // current sharing mode share_mode_t: 0 normal / 1 fast adjust / 2 follow PDO
    uint8_t  ocp_level;                 // OCP threshold: 0..3 = contract current x 110% / 120% / 130% / 140%
    bool     auto_epr;                  // X035 Auto EPR: enter EPR automatically when the charger supports it (voltage unchanged), on by default
    uint16_t target_mv[CH_COUNT];       // target voltage; 0 = not set (not pushed at power-on)
    share_profile_t profile;            // MANUAL feedforward learning result
} settings_t;

void settings_init(void);               // read record and restore accumulated energy; must be after meter_init
void settings_process(uint32_t now_ms); // call every second: delayed save and periodic energy save

const settings_t *settings_get(void);

void settings_set_sync(bool on);
void settings_set_auto_on(bool on);
void settings_set_balance(bool on);
void settings_set_balance_mode(uint8_t mode);
void settings_set_ocp_level(uint8_t level);
void settings_set_auto_epr(bool on);
void settings_set_target(ch_id_t ch, uint16_t mv);
void settings_set_profile(const share_profile_t *p);   // saved immediately

// Accumulated statistics read at power-on (0 if no record), used by stats as the base
void settings_get_lifetime(uint32_t *boots, uint32_t *on_s, uint32_t *trips);

// Delayed save (written even if nothing changed; used for accumulated statistics)
void settings_request_save(void);

// Write immediately (skip the delay), for operations that may be followed by a whole-board power loss (e.g. voltage switching)
void settings_save_now(void);

// Clear both channels' accumulated energy and save immediately
void settings_reset_energy(void);

// Diagnostics
uint32_t settings_seq(void);            // sequence number of the latest write (= total write count)
uint32_t settings_last_save_ms(void);   // 0 = not written since this power-on
bool     settings_error(void);          // latest write failed
bool     settings_loaded(void);         // a valid record was read at power-on
