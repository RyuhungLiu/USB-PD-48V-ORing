#pragma once

#include <stdbool.h>
#include <stdint.h>

// Minimal USB PD message decoding: PDO / RDO, used to derive the current contract voltage and current
// Reference USB PD R3.2 section 6.4.1 (PDO) / section 6.4.2 (RDO)

// Message header
#define PD_HDR_TYPE(h)      ((h) & 0x1F)
#define PD_HDR_NDO(h)       (((h) >> 12) & 0x07)
#define PD_HDR_EXT(h)       (((h) >> 15) & 0x01)

// Control messages (NDO = 0)
#define PD_CTRL_ACCEPT      0x03
#define PD_CTRL_REJECT      0x04
#define PD_CTRL_PS_RDY      0x06

// Data messages
#define PD_DATA_SRC_CAP     0x01
#define PD_DATA_REQUEST     0x02
#define PD_DATA_EPR_REQUEST 0x09

typedef enum pdo_kind_t
{
    PDO_KIND_NONE = 0,
    PDO_KIND_FIXED,
    PDO_KIND_BATTERY,
    PDO_KIND_VARIABLE,
    PDO_KIND_PPS,
    PDO_KIND_EPR_AVS,
    PDO_KIND_SPR_AVS,
} pdo_kind_t;

typedef struct pdo_info_t
{
    pdo_kind_t kind;
    uint16_t min_mv;        // = max_mv for fixed PDO
    uint16_t max_mv;
    uint16_t max_ma;        // 0 for EPR AVS (expressed as power)
    uint16_t pdp_w;         // EPR AVS only
} pdo_info_t;

pdo_info_t pd_decode_pdo(uint32_t pdo);

// Derive the requested voltage/current from the RDO and the PDO it points to
// Fixed/variable PDO use the PDO voltage; PPS uses the RDO output voltage (20mV); AVS uses the RDO output voltage (25mV)
bool pd_decode_rdo(uint32_t rdo, const pdo_info_t *pdo, uint16_t *mv, uint16_t *ma);

static inline uint8_t pd_rdo_pos(uint32_t rdo) { return (uint8_t)((rdo >> 28) & 0x0F); }

const char *pd_pdo_kind_str(pdo_kind_t kind);
