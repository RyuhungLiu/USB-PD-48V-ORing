#pragma once

#include <stdint.h>

// Boot target (written by V203 with SET_TARGET PERSIST): one record stored in the last Flash page (256B)
//   Fine adjustments for current balancing do not carry PERSIST and do not write Flash

typedef struct nv_target_t
{
    uint8_t  mode;
    uint8_t  flags;
    uint16_t mv;
    uint16_t ma;
} nv_target_t;

void nv_init(void);

// Returns NULL if there is no valid record
const nv_target_t *nv_target(void);

// Does not write if the content is identical
void nv_save_target(const nv_target_t *t);
