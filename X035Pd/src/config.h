#pragma once

// X035Pd firmware version (reported in INFO; the main controller uses it to recognize the new protocol)
#define FW_VERSION_MAJOR    2
#define FW_VERSION_MINOR    2
#define FW_VERSION_PATCH    3

// Keep contract at boot: VBUS above this value means the charger is still in a previous explicit contract (vSafe5V max is 5.5V)
#define CFG_RESUME_VBUS_MV          5800
// Above this value it is treated as EPR mode (SPR max 20V + margin)
#define CFG_RESUME_EPR_VBUS_MV      21500
// Number of attempts to keep the existing contract (MessageID increments each time to avoid the partner's duplicate detection)
#define CFG_RESUME_TRIES            3

// Attach detection (VBUS)
#define CFG_VBUS_PRESENT_MV         4000    // near the vSafe5V lower limit
#define CFG_VBUS_DISCONNECT_MV      3500    // vSinkDisconnect
#define CFG_DETACH_DEBOUNCE_MS      20

// Upper limit for waiting for Accept after Select_Capability (spec tSenderResponse 27-33ms). Measured 2026-10-25 on this board with two 240W chargers:
// with the other channel at full load, requesting EPR AVS +100mV on this unloaded channel occasionally took over 30ms to get Accept; sending Hard Reset per spec would cut power on that channel
// for about 2.5s. Relaxed to 100ms; an Accept that arrives after PD_tSenderResponse is reported with the ACCEPT_LATE event carrying the actual delay
#define CFG_ACCEPT_TIMEOUT_MS       100

// Main loop periodic tasks
#define CFG_VBUS_SAMPLE_MS          5
#define CFG_TC_PERIOD_MS            10
