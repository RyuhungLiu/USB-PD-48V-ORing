#pragma once

// CH32X035 USBPD physical layer: BMC TX/RX (CRC generated / appended by hardware, DMA TX/RX buffers), CC comparators
//   - Reception completes in the interrupt: after checking, GoodCRC is replied immediately (within tTransmit); messages are queued for the main loop
//   - Transmission completes blocking in the main loop: after sending, GoodCRC is polled (tReceive); retries are handled by the protocol layer

#include <stdbool.h>
#include <stdint.h>

#define PHY_MSG_MAX_BYTES   34      // header 2 + data 28 + CRC 4

typedef enum phy_sop_t
{
    PHY_SOP = 0,
    PHY_HARD_RESET,
} phy_sop_t;

typedef struct phy_rx_msg_t
{
    uint8_t len;                    // excluding CRC
    uint8_t buf[PHY_MSG_MAX_BYTES];
} phy_rx_msg_t;

typedef enum phy_rp_t
{
    PHY_RP_NONE = 0,
    PHY_RP_DEFAULT,                 // Default USB
    PHY_RP_1A5,                     // 1.5A (PD 3.x: SinkTxNG)
    PHY_RP_3A0,                     // 3.0A (PD 3.x: SinkTxOk)
} phy_rp_t;

typedef struct phy_stats_t
{
    uint16_t rx_msgs;
    uint16_t rx_crc_errors;         // software CRC check mismatches (when PHY_RX_CRC_CHECK is 0 they are only counted, not dropped)
    uint16_t rx_overflows;
    uint16_t tx_no_goodcrc;
    uint16_t hard_resets_rx;
} phy_stats_t;

void phy_init(void);

// Detect CC while unattached: bit0 = Rp on CC1, bit1 = Rp on CC2
uint8_t phy_cc_detect(void);

// Select the CC used for communication (1 / 2) and start receiving
void phy_attach(uint8_t cc);
void phy_detach(void);

// ErrorRecovery: remove the sink terminations. The board's Rd is an external 5.1k that cannot be disconnected, so instead GPIO drives both CC lines to 3.3V,
// above the vOpen threshold of every Rp level, so the source sees it as unattached; release restores the PHY inputs and comparators
void phy_cc_open(void);
void phy_cc_release(void);

// Read the Rp level on the selected CC while attached (briefly switches the comparator threshold, samples several times)
phy_rp_t phy_rp_level(void);

bool phy_rx_pop(phy_rx_msg_t *msg);
bool phy_hard_reset_received(void);     // cleared on read

// Send one SOP message (buf = header + data, excluding CRC), wait for GoodCRC with a matching MessageID;
// returns true = GoodCRC received
bool phy_transmit(const uint8_t *buf, uint8_t len, uint8_t message_id);

void phy_transmit_hard_reset(void);

const phy_stats_t *phy_stats(void);
