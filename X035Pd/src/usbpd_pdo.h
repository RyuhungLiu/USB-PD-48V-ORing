#pragma once

// Source PDO parsing (spec 6.4.1) and Request Data Object generation (6.4.2)

#include <stdbool.h>
#include <stdint.h>
#include "usbpd_def.h"

typedef struct pd_pdo_t
{
    pd_pdo_kind_t kind;
    uint16_t min_mv;        // fixed PDO = max_mv
    uint16_t max_mv;
    uint16_t max_ma;        // SPR AVS: current for the 20V range; EPR AVS: 0 (expressed by pdp_w)
    uint16_t max_ma_15v;    // SPR AVS: current for the 9..15V range
    uint16_t pdp_w;         // EPR AVS
} pd_pdo_t;

pd_pdo_t pd_pdo_decode(uint32_t pdo);

// Maximum current this PDO can supply at mv; returns 0 if the voltage is not covered
uint16_t pd_pdo_max_ma_at(const pd_pdo_t *p, uint16_t mv);

// PDO selection result
typedef struct pd_select_t
{
    uint8_t       pos;      // 1-based
    pd_pdo_kind_t kind;
    uint16_t      mv;
    uint16_t      ma;
    uint32_t      rdo;
    uint32_t      pdo;      // must be attached to EPR_Request
    bool          clamped;  // voltage / current was adjusted
} pd_select_t;

#define PD_SELECT_FIXED_FIRST   0
#define PD_SELECT_ADJ_FIRST     1

// caps: source capabilities (SPR 1..7; in EPR mode 1..11, where 1..7 is the SPR part)
// Returns false = no usable PDO
bool pd_select(const uint32_t *caps, uint8_t n, uint8_t mode, uint16_t mv, uint16_t ma, pd_select_t *out);

// PDO in the source capabilities closest to mv (used when keeping the contract at boot with no stored target)
bool pd_select_nearest(const uint32_t *caps, uint8_t n, uint16_t mv, pd_select_t *out);
