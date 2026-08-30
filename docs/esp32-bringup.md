# ESP32 bring-up (S3, C5, and C6)

The pin map's source of truth is
`ports/esp32/components/ultrawidelock_uwb/port/board_pins.h`. Change it there,
change it here.

## 1. Wire the radio

### DWM3000EVB shield

Power the EVB from the ESP32 board's **3V3** pin, not 5 V: the DW3000 is a 3.3 V
part. Share a common ground and USB-power the board during bring-up.

| DWM3000EVB pin | Signal | ESP32-S3 | ESP32-C5 | ESP32-C6 |
|---|---|---|---|---|
| D13 | SCLK | GPIO12 | GPIO8 | GPIO6 |
| D11 | MOSI | GPIO11 | GPIO9 | GPIO7 |
| D12 | MISO | GPIO13 | GPIO23 | GPIO2 |
| D10 | CS | GPIO10 | GPIO10 | GPIO10 |
| D8 | IRQ | GPIO5 | GPIO5 | GPIO3 |
| D7 | RSTn | GPIO4 | GPIO4 | GPIO1 |
| D9 | WAKEUP | GPIO6 | GPIO6 | GPIO0 |
| 3V3 | power | 3V3 | 3V3 | 3V3 |
| GND | ground | GND | GND | GND |
| D1 | SPI-POL | GND (mode-0 strap) | same | same |
| D0 | SPI-PHA | GND (mode-0 strap) | same | same |

Mode-0 strap: the DW3000 SPI must run CPOL=0/CPHA=0. Tie D0 and D1 to GND unless
your EVB revision already fixes the mode on the shield; check the EVB manual
before soldering.

GPIO 4, 5, 6 and 10-13 are clear of the S3's octal PSRAM pins. SPI2 routes
through the GPIO matrix, so the bus can be remapped in `board_pins.h`.

Why the C5 data pins differ: on the C5, GPIO11/12 are the UART0 console and
GPIO13 is USB-Serial-JTAG; GPIO8/9/23 avoid the strapping pins (2/7/25/27/28,
plus 3/26 per the DevKitC-1 guide) and the GPIO27 RGB LED. The C5 build targets the
4 MB flash floor of the WROOM-1 family (`partitions_4mb.csv`); if your module has
more, raise `CONFIG_ESPTOOLPY_FLASHSIZE` in `sdkconfig.defaults.esp32c5`.

Why the C6 pins differ: GPIO6/7/2 are SPI2's direct IO_MUX
SCLK/MOSI/MISO pins. GPIO10/1/3/0 keep the control signals away from the C6
strapping pins (GPIO4/5/8/9/15), native USB (GPIO12/13), and UART0
(GPIO16/17). The C6 reader and Matter configs target the official
ESP32-C6-DevKitC-1's 8 MB flash.

Sources: [ESP32-C6 SPI2 pins](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32c6/api-reference/peripherals/spi_master.html)
and [ESP32-C6-DevKitC-1 pin/flash guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32c6/esp32-c6-devkitc-1/user_guide.html).

### BU04 module in direct-SPI mode

The same C6 firmware can drive the DW radio inside a BU04 directly. The onboard
STM32F103 shares the DW SPI and control nets, so tie BU04 pad 2 (`ST_NRST`) to
GND before powering either board and keep it low for the whole session.
Releasing `ST_NRST` while the ESP32 drives those nets lets both MCUs drive the
bus.

| BU04 module pad | Signal | ESP32-C6-DevKitC-1 |
|---|---|---|
| 13 `SPI_CLK` | SCLK | GPIO6 |
| 14 `SPI_MOSI` | MOSI | GPIO7 |
| 15 `SPI_MISO` | MISO | GPIO2 |
| 16 `SPI_CSN` | CS | GPIO10 |
| 17 `IRQ` | IRQ | GPIO3 |
| 12 `DW_RSTN` | RSTn | GPIO1 |
| 11 `DW_WAKEUP` | WAKEUP | GPIO0 |
| 20 `GPIO5/SPIPOL` | SPI mode strap | GND |
| 21 `GPIO6/SPIPHA` | SPI mode strap | GND |
| 1 or 10 | ground | GND |
| 23 `VDD1` and 24/34 `3V3` | power domains | regulated 3.3 V |
| 2 `ST_NRST` | hold onboard STM32 in reset | GND for the entire session |

The BU04 specification recommends a 3.3 V supply capable of at least 500 mA.
Do not use 5 V on these module power pins. `DW_RSTN` is open-drain: the ESP
driver only pulls it low and releases it; never add an external pull-up to a
different voltage.

Source: [Ai-Thinker BU04 specification](https://ai-thinker.com/Uploads/file/20240927/20240927190544_82504.pdf).

This first image selects the existing DW3000-family driver profile. Capture the
raw `DEV_ID` line at boot: IDs ending `...02`/`...12` use the current profile,
`...04`/`...14` need the in-tree DW3720 profile before ranging.

### Check the EVB power-select jumper first

Correct wiring is not enough. With the jumper on the wrong source, SPI fails
silently: no valid device ID, a responder that never listens, and it looks like
a software fault.

## 2. Build and flash

```bash
cd examples/esp32/reader
make esp-set-target TARGET=esp32c6   # or esp32s3 / esp32c5
make build
make flash
make monitor
```

`make esp-set-target` runs `idf.py set-target` once per checkout. ESP-IDF is
expected at `~/esp/esp-idf`; override with `IDF_EXPORT=`. The port is
auto-detected and SEGGER/J-Link ports refused; `make ports` lists what is
attached and how each is classified.

### Toolchain

```sh
make esp-bootstrap APP=reader     # ESP-IDF only, about 5 GB
make esp-bootstrap                # ESP-IDF + esp-matter, about 20 GB and an hour
```

Neither is pinned by the build. Both default under `$HOME/esp`, overridden with
`IDF_EXPORT` and `ESP_MATTER_PATH`; an existing install keeps working. From
nothing, `esp-bootstrap` installs what the bench builds against: ESP-IDF v5.5.4
and esp-matter `93b1680`. `IDF_VER` and `ESP_MATTER_REV` choose others.

## 3. Expected output

The bench app brings the radio up, binds a canned URSK, and starts the CCC DS-TWR
responder:

    I (xxx) ultrawidelock_esp32: app_responder_start() = 0 (DW3000 up, responder listening)

- `= 0`: SPI, DW3000 and the CCC init path all came up. With no peer present
  there are no range lines.
- `= <nonzero> (FAILED -- check wiring/SPI)`: the DW3000 did not answer. In
  order, recheck the power-select jumper, then CS/SCLK/MOSI/MISO, then the mode-0
  strap, then set `ULTRAWIDELOCK_DW3000_SPI_FAST_HZ` to `2000000` in
  `board_pins.h`.

## 4. Prove a real range

Ranging needs a peer to drive the DS-TWR exchange: an Aliro-capable iPhone with a
key provisioned for this reader, or a second DW3000 board as initiator. With a
peer, `range: NN cm` lines appear and `status` reports a trusted range.

For the full approach-unlock path (commissioning, a key in the wallet, the Wallet
unlock animation) use [`apps/esp32-matter-lock`](../apps/esp32-matter-lock). This
bench app has no Matter layer, so nothing provisions a real credential for it.

No antenna calibration was needed on this hardware. If distances come out
negative or absurd, read [`docs/esp32-gotchas.md`](esp32-gotchas.md) §6.4 before
reaching for a calibration constant: it was a timestamp-pairing bug, not a
physical offset.

ESP32-S3 is hardware-validated with the DWM3000EVB. ESP32-C6 is
hardware-validated with the BU04 in direct-SPI mode and `ST_NRST` held low.
ESP32-C5 has build/release support but no recorded hardware validation.
