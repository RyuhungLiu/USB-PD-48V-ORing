#pragma once

// USB PD protocol layer (spec 6.12 Protocol Layer):
//   PRL_Tx: MessageIDCounter, nRetryCount retries
//   PRL_Rx: MessageID duplicate detection, counter reset on Soft_Reset
//   Chunked extended message reception: automatically sends Chunk Request and reassembles (EPR_Source_Capabilities can reach 44 bytes)

#include <stdbool.h>
#include <stdint.h>
#include "usbpd_def.h"

#define PRL_EXT_BUF_BYTES   64

typedef struct prl_msg_t
{
    uint16_t header;
    uint8_t  type;
    uint8_t  num_do;
    uint8_t  spec_rev;
    bool     extended;
    uint32_t obj[PD_MAX_DATA_OBJECTS];      // data objects of a data message
    uint16_t ext_size;                      // extended message: full data length
    uint8_t  ext[PRL_EXT_BUF_BYTES];        // extended message: reassembled data
} prl_msg_t;

void prl_reset(void);                       // after attach, Soft Reset, Hard Reset
void prl_set_spec_rev(uint8_t rev);
uint8_t prl_spec_rev(void);

// Called by the main loop: fetch one complete message (deduplicated, reassembled); returns false = none
bool prl_rx(prl_msg_t *msg, uint32_t now_ms);

// Send: returns true = GoodCRC received
bool prl_tx_ctrl(pd_ctrl_msg_t type);
bool prl_tx_data(pd_data_msg_t type, const uint32_t *obj, uint8_t n);
bool prl_tx_ext_ctrl(pd_ext_ctrl_t type, uint8_t data);
bool prl_tx_ext(pd_ext_msg_t type, const uint8_t *data, uint8_t size);   // single chunk (<= 26 bytes)

void prl_tx_hard_reset(void);
