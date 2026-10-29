# ATX_PSU — USB-C PD dual-input ATX power supply

Integrates the 48V10+10A ORing main board, the X9-ATX-500 DC-ATX module, a 2.0" display and a touch key into a standard ATX power supply enclosure. The AC input on the original rear panel (a C14 inlet on the power supply; C13 on the power cord end) is replaced by two USB-C ports (PD 3.1 EPR 48V).

## System architecture

```
USB-C #1 ─┐                                                ┌─ ATX 24P, EPS 4+4P, PCIe 6+2P ×2, SATA ×3, Molex 4P
          ├─ ORing main board ── 48V (XT60) ── X9-ATX-500 ─┤
USB-C #2 ─┘  X035 ×2 PD sink                               └─ J3 4P PWM fan
             LTC4359 ×2 (REV2)
             V203 main controller ─┬─ ST7789 2.0" display (GMT020-02-8P)
                                   └─ TTP223 touch
```

## Key specifications

| Item | Value | Notes |
|---|---|---|
| X9 input | 16–60 V, optimized for 48 V | OVP 60.2–60.5 V |
| X9 output | 500 W total; 12 V 40 A (peak 45 A ≤ 60 s) | Details in [docs/X9-ATX-500.md](docs/X9-ATX-500.md) |
| X9 input connector | 6P C6P (same spec as Molex 39-01-2060) | pin 1–3 positive, pin 4–6 negative |
| X9 size | 125 × 63 × 31 mm | Mounts on SFX power supply base plate holes |
| USB-C source | EPR 48 V 5 A = 240 W per channel | 480 W input for both channels combined |
| ORing main board | 91 × 56 mm, XT60 output | Silkscreen 6 A/CH, 12 A TOTAL |
| ATX enclosure | Nominal 150 × 86 × 140 mm (W × H × D) | Rear panel #6-32 mounting holes (viewed from outside): (6, 80), (144, 80), (6, 16), (120, 6) |

## Power budget

| Case | X9 input (48 V) | Available on 12 V side (96% efficiency) |
|---|---|---|
| Both USB-C | 480 W, about 10 A | About 460 W |
| Single USB-C | 240 W, about 5 A | About 230 W |

The system limit is set by the USB-C sources, not the X9. About 10 A on the 48 V side: 16 AWG or thicker wire recommended; C6P has 3 pins per pole, about 3.3 A per pin.

## Open items

1. **Enclosure source**: strip the steel case from an existing ATX power supply (keeping the fan and rear panel holes), or 3D print an ATX-size enclosure.
2. **Display location**: rear panel (seen from the back of the PC) or side (visible through a side-window case).
3. **Switch**: whether to keep the rocker switch originally next to the C14 (e.g. to cut the X9's 48 V), or leave everything to the touch key.
4. **X9 monitoring**: whether to connect J1 UART (115200 8N1, including input current and rail voltages) to the V203 for display. The V203's USART1/USART2 are already connected to the two X035s, and PB10 of USART3 is already taken by TOUCH; need to confirm a remaining UART or change pins.
5. **Fan control**: controlled by the X9's J3, or by the V203 based on temperature.

## Known risks

- **Single-channel dropout**: with a load above about 230 W, if either PD channel renegotiates or is unplugged, the other channel is overloaded, the X9 input goes undervoltage and the PC loses power. The current-sharing firmware has to account for this case.
- **Power-up sequence**: while PD negotiates from 5 V to 48 V, VIN passes through intermediate levels such as 20 V and 28 V, and the X9 may start above 16 V. The ORing output must only be turned on after the 48 V contract is established (LMx_EN).
- **Inrush current**: the inrush into the X9's large input capacitance at the moment ORing turns on is not documented in the manual and may trip the PD source's overcurrent protection. Needs measurement; add soft start if necessary.
- **Main board revision**: REV1's LM74700 cannot turn the output off; this project uses REV2 (LTC4359) directly.
- **Cooling**: the X9 needs forced air cooling at full load, with linear derating above 40 °C. The ORing main board's MOSFETs and shunt resistors also need to be in the airflow path.

## Mechanical (mechanical/)

3D-printed ATX-size enclosure, keeping the purple/green color scheme. Generator script `mechanical/gen_atx_case.py`; PCB reading is shared with the slanted enclosure via `../Enclosure/epro_pcb.py`.

```
"C:\Program Files\FreeCAD 1.1\bin\freecadcmd.exe" mechanical\gen_atx_case.py
```

Two versions; print one depending on the power supply mounting direction (the base plate is the same):

| Version | Use | Display module | Files |
|---|---|---|---|
| `fan_up` | Fan facing up (desktop, top-mounted power supply) | 0°, header on the left | `out/fan_up/print/`, `ORing_ATX_PSU_fan_up.FCStd` |
| `fan_down` | Fan facing down (bottom-mounted power supply in the case): when the whole unit is mounted upside down, the rear panel is flipped vertically | Rotated 180°, header on the right; once installed the image is upright, no firmware change needed | `out/fan_down/print/`, `ORing_ATX_PSU_fan_down.FCStd` |

| Part | Color | Print orientation | File |
|---|---|---|---|
| Top shell (top, four walls and rear panel in one piece) | Purple | Top fan grille facing down | `1_shell.stl` |
| Base plate | Green | Bottom face down | `2_base.stl` |

Layout (viewed from outside the rear panel):

- Two USB-C on the rear panel (X 21.2 / 60.7, Z 18.2): main board raised on 12 mm standoffs, so when mounted upside down for bottom exhaust the openings start 14.6 mm from the top edge of the power supply, clear of the case opening frame; the display (42 × 33 window, 45° chamfer on the outside) and touch key are centered horizontally as a group, window center (62.5, 50); vertical vent slots on both sides.
- Top honeycomb holes at 10 mm pitch, 8.2 mm across flats.
- Inside: ORing main board on the left (USB edge against the rear panel), X9 on the right, 120 mm fan on top.
- X9 orientation is set by `X9_ROT` (affects only the base plate; the top shell is unchanged):
  - `0`: long edge of the output connectors faces the right wall, P5 / P6 face the front.
  - `180` (current default): rotated half a turn about its center, long edge of the output connectors faces the ORing main board, P5 / P6 near the rear panel, J1 / J3 face the front. The X9 is moved 2 mm forward so the P5 plug clears the (120, 6) nut boss on the rear panel. Base plate X9 countersunk holes (X, Y, with Y measured from the front wall): (77.8, 17.8), (103.2, 17.8), (77.8, 131.2), (103.4, 131.2).
  - After rotating 180°, P5 is near the rear panel (about (120, 18)) and the ORing XT60 is at (41, 100), a straight-line distance of about 115 mm; an XT60 → P5 cable of 20 cm or more is recommended.
- U-shaped cable exit on the right side of the front wall, vent slots on the left.

Purchased parts:

| Item | Qty | Use |
|---|---|---|
| #6-32 heat-set brass insert (OD about 5 mm, length 6.4 mm) | 4 | Rear panel ATX standard mounting holes |
| M3 countersunk screw | 9 | Base plate screwed from below into the top shell (5) and the X9 back plate (4) |
| M3 self-tapping screw | 4 | ORing main board to base plate standoffs |
| M2 self-tapping screw | 4 | Display module |
| 120 mm PWM fan + fan screws | 1 | Connected to X9 J3 |

To be confirmed:

- **X9 back plate holes**: taken from the manual's mechanical drawing, choosing 4 holes that are mirror-symmetric left and right; confirm with the actual part.
- **#6-32 heat-set inserts**: adjust `INSERT_D` to the actual size.
- **Clearance above the X9 connectors**: 40 mm (vertical connector + plug + cable bend); confirm with the actual cables. With `X9_ROT = 180`, the clearance zone of the long-edge connectors is about 7 mm from the nearest ORing component; check again if the plugs are wider than estimated.
- **Display orientation**: the `fan_down` version already rotates the module 180°; if the wrong version is installed, the image can also be flipped in firmware (ST7789 MADCTL 0x60 ↔ 0xA0).

## Folders

- `docs/`: specification summaries, measurement records
- `mechanical/`: enclosure generator script and output
