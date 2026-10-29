# Current-sharing control strategy (v0.0.71)

Corresponding code: `process_run()` in `src/power/share.c`; parameters are in `src/config.h`.

This is not a conventional PID but an **integral-type step controller with adaptive gain**:
- The output is an "offset voltage"; each step adds an increment to the existing offset, equivalent to the I term. There are no P or D terms.
- Step size = error ÷ online-estimated gain k × gain percentage (equivalent to a variable Ki).
- In addition, a dead band, predictive hysteresis and transient freeze are added to deal with the quantized steps of PPS/AVS and the slow response of the chargers.

## 1. Control structure

```mermaid
flowchart LR
    SP["Target split<br/>NORMAL/FAST: total / 2<br/>FOLLOW: in proportion to the two channels' contract currents"] --> SUM(("err = I1 − target"))
    INA["INA237 current<br/>current_ma_avg (about 80ms low-pass)"] --> SUM
    SUM --> DZ{"Dead band / hysteresis"}
    DZ -->|adjustment needed| STEP["Step = |err| ÷ k × gain%<br/>clamped, quantized (PPS 20mV / AVS 100mV)"]
    STEP --> ACC["Offset accumulation (integral)<br/>off[ch] ± step, limited by lim_lo/hi"]
    ACC --> PD["X035 SET_TARGET ADJ_FIRST<br/>v0 + off[ch]"]
    PD --> CHG["Charger PPS / AVS output"]
    CHG --> PLANT["Path resistance of both channels + ideal diodes<br/>(plant)"]
    PLANT --> INA
    PLANT -. measured Δerr / Δv .-> K["Online-estimated gain k (mA/V)"]
    K -.-> STEP
    K -.-> DZ
```

## 2. Each decision cycle (RUN)

```mermaid
flowchart TD
    A([RUN cycle]) --> L{"Any channel dropped?<br/>link down / contract invalid / path shut off by protection"}
    L -->|yes| HOLD[["Enter HOLD: freeze offset, wait for recovery"]]
    L -->|no| C{"Contract types still correct?<br/>adjustable channel still on PPS/AVS, anchor channel still on a fixed level"}
    C -->|no| EXIT[["Exit current sharing"]]
    C -->|yes| P{"Previous step still waiting for a reply?"}

    P -->|rejected| R1["Revert offset, narrow that direction to the original value<br/>fails++, 3 in a row → exit"]
    P -->|no new contract in 1.5s| R2["Revert offset, fails++"]
    P -->|confirmed| S["Clear fails<br/>settle = PPS 300ms / AVS 1500ms"]
    P -->|none| W
    S --> W{"OCP assist (urgent)?"}
    W -->|no| W2{"Settle time elapsed, and time since last decision<br/>≥ 500ms (FAST 100ms)?"}
    W2 -->|no| END([End of this cycle])
    W -->|yes| SYNC
    W2 -->|yes| SYNC["Sync offset to actual contract voltage − v0"]

    SYNC --> M["Read I1, I2 averages<br/>err = I1 − target, total = I1 + I2"]
    M --> T{"Total current change > 30%?<br/>(sudden load / sudden unload)"}
    T -->|yes| TF["Transient freeze: no adjustment, no k learning<br/>history action 4"] --> END
    T -->|no| MIN{"total < 500mA?"}
    MIN -->|yes| END
    MIN -->|no| KE

    subgraph KL["Gain learning (when the previous step was sent)"]
        KE{"Previous step's effect usable?<br/>total current change < 20%<br/>error actually reduced, Δv > 0<br/>both channels ≥ 15% of total current before and after"}
        KE -->|yes| KU["km = error reduction ÷ Δv<br/>first time: k = km<br/>afterwards: k = (k + km) / 2<br/>limited to 1-40 A/V"]
        KE -->|no| KN["Do not update k (initial value 5 A/V)"]
    end

    KU --> OL
    KN --> OL
    OL["over_limit = higher-current channel > 97% of contract current<br/>or, during OCP assist, that channel > 90% of OCP threshold"]
    OL --> D{"Not over_limit and<br/>2|err| ≤ dead band × total?<br/>Dead band NORMAL 10% / FAST 5% / FOLLOW 4%"}
    D -->|yes| OK["Hold, history action 0"] --> END
    D -->|no| ST["Step s = |err| ÷ k × gain%<br/>NORMAL/FOLLOW 50%, max 100mV<br/>FAST 80%, max 200mV<br/>OCP assist: voltage needed to reach 90% of threshold, max 300mV"]

    ST --> SEL{"Select channel to act on"}
    SEL -->|can lower and offset sum > 0, or cannot raise| LOW["Lower the higher-current channel"]
    SEL -->|can raise| RAISE["Raise the lower-current channel"]
    SEL -->|both at limit| SAT["Saturated, history action 3"] --> END
    LOW --> Q
    RAISE --> Q
    Q["Quantize: PPS 20mV / AVS 100mV<br/>move at least one quantization step"] --> PR{"Predictive hysteresis (when not over_limit)<br/>k × Δv > 160% × |err|?"}
    PR -->|yes: would only flip to the other side| HOLDPOS["No move, revert to original offset, history action 0"] --> END
    PR -->|no| SEND["Record err0, total0, smaller channel current, Δv (for learning k next time)<br/>send SET_TARGET ADJ_FIRST v0 + off<br/>history action 1/2 (CH1/CH2)"]
    SEND --> END
```

## 3. Main parameters

| Parameter | Value | Purpose |
|---|---|---|
| `SHARE_PERIOD_MS` / `SHARE_FAST_PERIOD_MS` | 500 / 100 ms | Decision period |
| `SHARE_SETTLE_MS` / `SHARE_SETTLE_AVS_MS` | 300 / 1500 ms | Wait for current to settle after a new contract is confirmed |
| `SHARE_DIFF_PCT` / `FAST` / `FOLLOW` | 10 / 5 / 4 % | Dead band (2\|err\| relative to total current) |
| `SHARE_GAIN_PCT` / `SHARE_FAST_GAIN_PCT` | 50 / 80 % | Fraction of the required voltage taken as the step |
| `SHARE_STEP_MAX_MV` / `FAST` / `URGENT` | 100 / 200 / 300 mV | Maximum single step |
| `SHARE_STEP_MV` / `SHARE_AVS_STEP_MV` | 20 / 100 mV | Quantization step (PPS / AVS) |
| `SHARE_K_INIT/MIN/MAX_MA_PER_V` | 5 / 1 / 40 A/V | Gain initial value and range |
| `SHARE_K_EST_MIN_PCT` | 15 % | Minimum share of each channel when learning k (excludes the diode cutoff region) |
| `SHARE_PREDICT_PCT` | 160 % | Predictive hysteresis: no move if k × Δv exceeds this percentage of \|err\| |
| `SHARE_TRANSIENT_PCT` | 30 % | Transient freeze threshold |
| `SHARE_LIMIT_PCT` | 97 % | Near the contract limit, dead band and hysteresis do not apply |
| `SHARE_RANGE_MV` / `SHARE_FOLLOW_DOWN_MV` | 1000 / 1500 mV | Offset range (also limited by the protection window and the level range) |
| `SHARE_ACK_TIMEOUT_MS` / `SHARE_MAX_FAILS` | 1500 ms / 3 | Reply timeout and maximum consecutive failures |

## 4. Known limitations

- EPR AVS only has 100mV steps. When the path resistance is very small (measured k ≈ 14-16 A/V), a single step shifts about 1.6A, so at a 4A load the two channels can only reach a split error of about ±40% at best. The firmware stops at whichever of the two nearest states is predicted to be better and does not oscillate back and forth.
- Ways to improve: add about 25mΩ of current-sharing resistance in series with each channel to lower k; or use a voltage that has PPS (20mV steps).
