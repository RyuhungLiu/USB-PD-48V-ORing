# X035Pd ⟷ V203 UART Protocol

USART4 (PB0 TX / PB1 RX), 921600 8N1. The X035 is the slave: it only handles PD Sink negotiation and reporting; all policy is issued by the V203.

## Frame format

| Byte | Field | Description |
|---|---|---|
| 0 | SOF | Fixed `0xA5` |
| 1 | LEN | Payload length 0-60 |
| 2 | SEQ | Sequence number. V203 requests increment it on their own (1-255) and the X035 echoes it back in the reply; frames sent unsolicited by the X035 use 0 |
| 3 | TYPE | Frame type |
| 4… | PAYLOAD | LEN bytes; multi-byte fields are always little-endian |
| 4+LEN | CRC8 | Polynomial 0x07, initial value 0x00, covering LEN, SEQ, TYPE, PAYLOAD |

## V203 → X035 (requests)

| TYPE | Name | Payload | Reply |
|---|---|---|---|
| 0x01 | GET_STATUS | — | STATUS |
| 0x02 | SET_TARGET | mode(1) flags(1) mV(2) mA(2) | RESULT |
| 0x03 | GET_SOURCE_CAPS | — | SOURCE_CAPS |
| 0x04 | HARD_RESET | — | RESULT |
| 0x05 | CONFIG | flags(1): bit0 forward PD messages, bit1 Auto EPR (2.1+, enabled by default at boot) | RESULT |
| 0x06 | REBOOT | — | RESULT, then reboots |
| 0x07 | GET_INFO | — | INFO |
| 0x08 | ERROR_RECOVERY | ms(2), 500 if omitted, range 25-5000 (2.2+) | RESULT |

### SET_TARGET

- **mode**
  - 0 = FIXED_FIRST: a fixed PDO with a matching voltage has priority, then PPS → SPR AVS → EPR AVS covering that voltage.
  - 1 = ADJ_FIRST: PPS → SPR AVS → EPR AVS have priority, then fixed PDOs.
- **flags**
  - bit0 PERSIST: written to the X035 Flash as the target kept at boot (including reboot after power loss).
  - bit1 NO_EPR: do not enter EPR mode for this target.
- **mV**: target voltage. Aligned to 20mV for PPS and 100mV for AVS.
- **mA**: operating current. 0 means use the maximum of that PDO at this voltage; above the maximum, the maximum is used and OK_CLAMPED is replied.
- When not attached, or while a negotiation is in progress, the target is held and applied once attached or once the negotiation completes. In this case the reply is OK (or NO_SOURCE).
- When EPR is needed (target > 20V and the charger's PDO1 indicates EPR support), the X035 enters EPR mode automatically.

### RESULT codes

| Code | Name | Description |
|---|---|---|
| 0 | OK | Accepted; see the contract fields of STATUS for the negotiation result |
| 1 | OK_CLAMPED | Accepted, but the voltage or current was adjusted into the usable range |
| 2 | NO_SOURCE | Not attached or no source capabilities yet (target is held) |
| 3 | NO_MATCH | No usable PDO in the source capabilities; the current contract is kept |
| 4 | BAD_ARG | Invalid argument |

## X035 → V203

| TYPE | Name | Payload |
|---|---|---|
| 0x81 | INFO | fw_major(1) fw_minor(1) fw_patch(1) reset_flags(1) nv_mode(1) nv_flags(1) nv_mV(2) nv_mA(2) |
| 0x82 | STATUS | See the table below |
| 0x83 | SOURCE_CAPS | flags(1): bit0 = EPR source capabilities; n(1); PDO u32 × n (n ≤ 11) |
| 0x84 | EVENT | code(1) arg(2) |
| 0x85 | RESULT | cmd_type(1) code(1) |
| 0x86 | PD_MSG | dir(1): 0 received, 1 sent; followed by the PD message (header + data, excluding CRC) |

- INFO: sent unsolicited once at boot.
- STATUS: sent when the status changes, otherwise once every 500ms (doubles as heartbeat).
- SOURCE_CAPS: sent unsolicited when new source capabilities are received.
- PD_MSG: forwarded only when CONFIG bit0 is enabled.

### STATUS payload (26 bytes in 2.1+; 2.0 has the first 24 bytes)

| Offset | Length | Field |
|---|---|---|
| 0 | 1 | tc: 0 not attached, 1 CC1, 2 CC2 |
| 1 | 1 | pe_state (`pe_state_t` number) |
| 2 | 1 | flags: bit0 contract present, bit1 EPR mode, bit2 PD 3.x, bit3 charger supports EPR, bit4 negotiating, bit5 target cannot be met, bit6 source capabilities available |
| 3 | 1 | Rp: 0 none, 1 Default, 2 1.5A, 3 3.0A |
| 4 | 2 | VBUS mV (X035 ADC) |
| 6 | 1 | Contract PDO position (1-based) |
| 7 | 1 | Contract kind: 0 none, 1 fixed, 2 battery, 3 variable, 4 PPS, 5 EPR AVS, 6 SPR AVS |
| 8 | 2 | Contract mV (requested voltage for PPS/AVS) |
| 10 | 2 | Contract mA (operating current) |
| 12 | 2 | Contract sequence number: incremented on each PS_RDY (including periodic PPS requests) |
| 14 | 1 | Hard Reset count (received + sent) |
| 15 | 1 | Source capabilities sequence number: incremented each time new source capabilities are received |
| 16 | 2 | Current target mV |
| 18 | 1 | Current target mode |
| 19 | 1 | Most recent negotiation result: 0 OK, 3 NO_MATCH, 6 Reject, 7 Wait, 8 EPR entry failed, 9 timeout |
| 20 | 2 | Receive CRC error count |
| 22 | 2 | Transmit failure count (no GoodCRC received) |
| 24 | 1 | EPR entry failure reason: 0xFF none, 0xFE rejected or not supported, otherwise EPR_Mode Data (1 cable does not support EPR ...) |
| 25 | 1 | flags2: bit0 Auto EPR enabled |

### EVENT codes

| Code | Name | arg |
|---|---|---|
| 1 | ATTACH | CC (1/2) |
| 2 | DETACH | — |
| 3 | HARD_RESET_RX | 2.2.1+: high 5 bits = PE state code when received, low 11 bits = ms spent in that state (max 2047); 0 in 2.2.0 and earlier |
| 4 | HARD_RESET_TX | 2.2.1+: high 5 bits = reason (see the table below), low 11 bits = ms spent in the previous state before issuing (max 2047); 0 in 2.2.0 and earlier |
| 5 | SOFT_RESET | 0 received, 1 sent, 2 sent because a request got no Accept (2.2.3+) |
| 6 | EPR_ENTERED | — |
| 7 | EPR_ENTER_FAILED | Failure reason (EPR_Mode Data) |
| 8 | REJECT | — |
| 9 | WAIT | — |
| 10 | RESUME_OK | Keeping the contract at boot succeeded |
| 11 | RESUME_FAILED | — |
| 12 | CONTRACT | Contract mV |
| 13 | ERROR_RECOVERY | Milliseconds the terminations are removed (2.2+) |
| 14 | ACCEPT_LATE | Milliseconds from sending the request to receiving Accept; reported only if over tSenderResponse (30ms) (2.2.2+). Wait limit is `CFG_ACCEPT_TIMEOUT_MS` (100ms); on timeout a Soft_Reset is sent first (2.2.3+, SOFT_RESET arg 2), and Hard Reset (reason NO_ACCEPT) only on another timeout before a contract is obtained |
| 15 | HARD_RESET_GIVEUP | 2.2.3+: HardResetCounter exhausted (2 consecutive Hard Resets without obtaining a contract); no more are sent, waits for source capabilities instead; arg same as HARD_RESET_TX. Since 2.2.3 HardResetCounter is cleared to zero each time a contract is obtained (previously only cleared on attach / main controller request; after two accumulated, a further timeout would silently stay in Wait_for_Capabilities, and in EPR the partner's keep-alive would time out after about 0.8s and it would send Hard Reset back) |

HARD_RESET_TX reasons (2.2.1+, `link_hr_reason_t`):

| Code | Name | Description |
|---|---|---|
| 1 | WAIT_CAP | SinkWaitCapTimer timeout (Soft_Reset already tried, or VBUS insufficient) |
| 2 | NO_ACCEPT | Select_Capability: no Accept/Reject received (30ms in 2.2.1 and earlier; 100ms since 2.2.2, see ACCEPT_LATE; since 2.2.3 the first time sends Soft_Reset first) |
| 3 | NO_PS_RDY | Transition_Sink: no PS_RDY within PSTransitionTimer (SPR 550ms / EPR 1020ms) |
| 4 | KEEPALIVE | EPR_KeepAlive: no Ack 3 times in a row |
| 5 | SOFT_RESET_TX | Failed to send Soft_Reset (no GoodCRC) |
| 6 | SOFT_RESET_NO_RESP | No Accept received after sending Soft_Reset |
| 7 | SOFT_RESET_ACK_TX | Soft_Reset received, failed to send the Accept reply |
| 8 | HOST | Requested by V203 (HARD_RESET 0x04) |

PE state codes (used by HARD_RESET_RX, same as the state in STATUS): 0 Startup, 1 Discovery, 2 Wait_for_Capabilities, 3 Evaluate_Capability, 4 Select_Capability, 5 Transition_Sink, 6 Ready, 7 Hard_Reset, 8 Transition_to_default, 9 Get_Source_Cap, 10 SNK_Soft_Reset, 11 Send_Soft_Reset, 12 Send_EPR_Mode_Entry, 13 EPR_Mode_Entry_Wait_For_Response, 14 EPR_Keepalive, 15 Send_EPR_Mode_Exit, 16 Detached.

## Keeping the contract at boot

When the X035 reboots after power loss and VBUS already has voltage (meaning the charger is still in the previous contract), it first tries to take over without cutting power:

- **VBUS > 21V (EPR mode)**: send EPR_KeepAlive; after receiving Ack, send EPR_Get_Source_Cap; after obtaining the EPR source capabilities, request the target with EPR_Request.
- **SPR mode**: send Get_Source_Cap; after obtaining the source capabilities, request the target with Request.

Each of the above is retried 3 times (MessageID increments to avoid the partner's duplicate detection). If it still fails, Soft_Reset is sent instead, and only if that also fails is Hard Reset sent.

Target requested on takeover:

1. The PERSIST target in Flash is used first.
2. If there is no record, choose the fixed PDO closest to the current VBUS, or an adjustable PDO covering the current VBUS.
3. If neither exists, request 5V.

## ERROR_RECOVERY: software replug (2.2+)

The ErrorRecovery state of the Type-C spec: the sink removes its CC terminations for at least tErrorRecovery (25ms); the source sees a detach, removes VBUS and resets the port. After the terminations are restored it reconnects and negotiates from 5V with new source capabilities, which is equivalent to unplugging and replugging the cable.

- The board's Rd is an external 5.1k that cannot be disconnected. Instead, the X035 uses GPIO to drive CC1/CC2 to 3.3V (above the vOpen threshold of every Rp level), and restores the PHY inputs after the specified milliseconds.
- If attached when executed, a DETACH event is sent first, then an ERROR_RECOVERY event; after recovery, ATTACH/CONTRACT are sent through the normal flow. The old contract is not kept.
- VBUS of this channel is cut. The X035 is powered from 3V3 derived from the two VIN channels diode-ORed, so it is unaffected while the other channel has power; **the two channels must not execute it at the same time**, otherwise the whole board loses power.
- It may still work when the charger has locked up and removed Rp (CC is 0 in STATUS): measured 2026-10-21, a 240W charger locked up with CC=0 and VBUS 0V recovered to EPR AVS 48V 2.3 seconds after executing ErrorRecovery. Try the software replug first, and unplug the cable only if it does not help.

## Auto EPR (2.1+)

When the charger's PDO1 indicates EPR support and the current contract is an SPR **fixed PDO**, the X035 enters EPR mode automatically:

1. Send EPR_Mode Enter.
2. After receiving EPR_Source_Capabilities, request the original PDO with EPR_Request, **voltage unchanged**.

After that, EPR_KeepAlive is sent every 0.3 seconds, and the V203 can read the complete EPR source capabilities.

- **Entry failure** (for example the cable does not support EPR): the reason is recorded and no more automatic attempts are made for this attach.
- **Request for SPR PPS/AVS rejected while in EPR mode**: the X035 first sends EPR_Mode Exit, obtains the SPR source capabilities and then requests again. Once the contract becomes an adjustable PDO, EPR is no longer entered automatically.
- **Keeping the contract at boot**: always send EPR_KeepAlive first (because VBUS may also be ≤ 20V in EPR mode). If the partner replies Not_Supported, or there is no response repeatedly, switch to Get_Source_Cap.

In addition, after receiving any source capabilities (including those sent unsolicited by the partner in the Ready state), a Request is always replied, even when the selected PDO is the same as the current contract. 2.0.0 skipped it in that case, causing the partner to time out and send Hard Reset; fixed in 2.1.0.
