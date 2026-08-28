# nRF5340 wiring

Every pin and connection on the nRF bench stack. Bring-up is
[nrf5340-bringup.md](nrf5340-bringup.md).

## Source of truth

[`../apps/nrf5340dk-lock/overlays/dw3000-nfc.overlay`](../apps/nrf5340dk-lock/overlays/dw3000-nfc.overlay)
is authoritative: the build compiles it and it cannot drift. This page adds what
a devicetree cannot carry, being Arduino header positions, far-end pin names,
power rails and stack order. **If the two disagree, the overlay wins.**

Resolved from the generated `build/matter-aliro-door-lock-app/zephyr/zephyr.dts`:

* Header positions from the `arduino_header` node's `gpio-map`, where map indices 0 to 5
  are A0 to A5 and 6 onward are D0 to D15.
* Peripheral claims by decoding each `psels` value, which encodes
  `(function << 24) | (port * 32 + pin)`.
* Chip selects and side-band lines from `cs-gpios`, `irq-gpios`, `reset-gpios` and
  `wakeup-gpios` on the device nodes.

## Stack order

```
X-NUCLEO-NFC12A1 (ST25R300)     NFC front end, SPIM1 @ 4 MHz
        |
Arduino Proto R3                upper patch panel
        |   9 jumpers
Arduino Proto R3                lower patch panel
        |
DWM3000EVB (DW3110)             UWB radio, SPIM4 @ 8 MHz, seated on the DK
        |
nRF5340 DK
```

The two proto shields are a patch panel. Nothing crosses between them except the jumpers
you fit, power included. That is the point: the DWM3000EVB occupies the native Arduino
SPI positions D10 to D13, so the NFC board cannot also use them and its signals are
re-routed to A2 to A5 on the way up.

The EVB also acts as the spacer that lifts everything above it clear of the DK's P5 and
P20 connectors. The vendor documents contact with those connectors as a cause of NFC
driver initialization failures.

## Every pin and every connection

Read each row left to right as the signal's path up the stack. The DWM Arduino pin is the
DK's own header position, carried up through the seated EVB onto the lower proto shield.
The step from the DWM Arduino pin to the X-NUCLEO Arduino pin is a jumper between the two
proto shields. A dash means the signal stops there.

| nRF pin | DWM3000EVB | DWM Arduino pin | X-NUCLEO Arduino pin | X-NUCLEO NFC12A1 |
|---|---|---|---|---|
| P0.04 | pass through | A0 | - | - |
| P0.05 | pass through | A1 | - | - |
| P0.06 | pass through | **A2** | **D13** | SCK |
| P0.07 | pass through | **A3** | **D11** | MOSI |
| P0.25 | pass through | **A4** | **D12** | MISO |
| P0.26 | pass through | **A5** | **D10** | CS, active low |
| P1.00 | pass through | D0 | - | - |
| P1.01 | pass through | D1 | - | - |
| P1.04 | pass through | D2 | - | - |
| P1.05 | pass through | D3 | - | - |
| P1.06 | pass through | D4 | - | - |
| P1.07 | pass through | **D5** | **D8** | RESET, active low, pull-down |
| P1.08 | pass through | **D6** | **A0** | IRQ, active high |
| P1.09 | RESET, active low | D7 | - | - |
| P1.10 | IRQ, active high | D8 | - | - |
| P1.11 | WAKEUP, active high | D9 | - | - |
| P1.12 | CS, active low | D10 | - | - |
| P1.13 | MOSI | D11 | - | - |
| P1.14 | MISO | D12 | - | - |
| P1.15 | SCK | D13 | - | - |
| P1.02 | pass through | D14 | - | - |
| P1.03 | pass through | D15 | - | - |
| rail | pass through | **3V3** | **3V3** | VDDIO |
| rail | pass through | **5V** | **5V** | VBUS |
| rail | pass through | **GND** | **GND** | ground |

Nine rows carry a bold pair. Those pairs are the nine jumpers, and nothing else on the
stack is wired by hand. Rows where the DWM3000EVB column names a function are the DW3000's
own seven signals, taken at the EVB and never carried further up.

Pins reserved by the SoC even though nothing on this stack uses them: P1.00 and P1.01 are
UART1 RX and TX, P1.02 and P1.03 are I2C1 SDA and SCL. Do not borrow D0, D1, D14 or D15.
Genuinely free: A0, A1, D2, D3, D4.

Two things this table exists to stop:

* **The four SPI jumpers cross headers.** The DWM side is analog (A2 to A5), the X-NUCLEO
  side is digital (D13, D11, D12, D10). A jumper that looks symmetrical is wrong.
* **The three power jumpers are not optional.** The proto shields pass no power through on
  their own.

The RESET jumper, DWM D5 to X-NUCLEO D8, is the one value here not taken from either the
overlay or a vendor wiring table. The vendor tables omit reset entirely, and `reset-gpios`
is optional in the `x-nucleo-nfc` binding. D8 on the NFC side is derived from the add-on's
own seated overlay, where reset sits on P1.10, which is D8. Confirm against the board
pinout before trusting it.

Off-header pins, listed so they are never mistaken for spares: UART0 console on P0.19 to
P0.22, LED1 PWM on P0.28, external QSPI flash on P0.13 to P0.18.

## Before powering

* 3.3 V only, never 5 V on the DW3000, and share a common ground with the DK.
* Set the DWM3000EVB power-select jumper before anything else. The wrong position makes
  SPI fail silently, with no device ID and a responder that never listens. This has cost
  multiple days of debugging on this project. Use the 3.3 V source and read the
  silkscreen next to the jumper block.

## Failure signatures

| Console line | Meaning |
|---|---|
| `RFAL: NFC initialization failed` | No SPI conversation with the ST25R300. Check the four SPI jumpers (A2 to A5), the RESET jumper (D5), and the three power jumpers. A loose IRQ jumper (D6) usually still lets init pass. |
| `NFC transport start failed` then `Failed to start Aliro: 1` | Follows the above. `AliroStart()` starts the NFC transport first and returns on failure, so BLE advertising never starts either and the reader is silent on air even though Matter still works. |
| `dwt_probe failed: -1, raw DEV_ID=0xffffffff` | No SPI conversation with the DW3110. Expected when the EVB is absent. Otherwise check the power-select jumper first, then the EVB's seated contacts on D7 to D13. |

Broader SPI and ranging failure modes: [troubleshooting.md](troubleshooting.md).
