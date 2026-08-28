# Power baseline

The DWM3001CDK's resting power state, read off a running board over SWD.

> Verification convention, as in [`dwm3001cdk-surgery.md`](dwm3001cdk-surgery.md):
> **VERIFIED** = observed on this silicon; **MEASURED** = a number read off the
> target, not estimated; **PREDICTED** = derived from source or spec and never
> observed here, so a hazard to measure rather than a finding.

Reproduce with:

```sh
./scripts/power-baseline.sh          # 20 samples, about 40 s
```

---

## 0. What this is, and the one thing it is not

**There is no current measurement here.** The nRF52833 has no current sense, no
PPK2 was on this machine, and the DWM3001CDK's on-board J-Link OB and LEDs share
the USB rail with the two ICs that matter, so a meter on the USB port would read
mostly debugger anyway.

What the script does read is **the set of registers that decide the current**:
which clocks are running, whether the DC/DC is on, which peripherals are enabled
with nobody using them, and what state the 2.4 GHz radio is actually sitting in.
Those are facts about the silicon, read while it runs. They move most of a power
review from PREDICTED to MEASURED for the price of an SWD attach.

The milliamp column in §3 is arithmetic on published datasheet figures against
that measured state. The states are MEASURED. The milliamps are PREDICTED. §5 is
how to falsify them.

**Non-invasive by construction.** Every access is a memory read over the AHB-AP.
The core is never halted and never reset, so the board keeps advertising, keeps
its Thread attachment, and keeps any session it had. The RTC1 liveness line is
the proof: a counter that advanced across two reads is a kernel that was not
stopped to take them.

---

## 1. Run 1, the board as found

**MEASURED 2026-08-28T20:18Z**, `probe-rs` 0.32.0 over the board's own J-Link OB,
chip `nRF52833_xxAA`. Board resting: no phone present, no session, no console
attached.

```
LIVENESS
  RTC1 COUNTER   0x00eb931a -> 0x00ebae45   6955 ticks = 212 ms elapsed

CLOCKS
  HFCLKSTAT      0x00010001   src=XTAL (HFXO)            running=1
  LFCLKSTAT      0x00010001   src=XTAL (LFXO)            running=1
  LFCLKSRC       0x00000001

REGULATOR
  POWER.DCDCEN   0x00000001   REG1 DC/DC ENABLED (optimal)
  POWER.DCDCEN0  0x00000000   REG0 (high voltage) off / bypassed

PERIPHERAL ENABLE
  UART0/UARTE0   0x00000004   ENABLED as legacy UART
  TWI/SPI0       0x00000006   TWIM (I2C, EasyDMA)
  UARTE1         0x00000000   TWI/SPI1 0   SPIM2 0   SPIM3 0   USBD 0   SAADC 0

RADIO
  MODE           0x0000000f   Ieee802154_250Kbit
  FREQUENCY      0x0000004b   2475 MHz  (802.15.4 channel 25)
  TXPOWER        0x00000000   0 dBm

SAMPLING (20 samples)
  RADIO in RX    20/20 samples  = 100% duty
  HFXO running   20/20 samples  = 100% duty

IMAGE  (MCUboot header at 0x0a000)
  image size     407544 B
  version        0.3.1
```

## 1.1 Run 2, after flashing v0.4.0: every register identical

**MEASURED 2026-08-28T20:37Z**, same board, after
`make build RELEASE=1 SMP=1` and `make flash` (never `flash-erase`).

Every line above is unchanged. `MODE = 0x0F`, `FREQUENCY = 0x4B`, RADIO in RX
**20/20**, HFXO **20/20**, `DCDCEN = 1`, `UART0.ENABLE = 4`, app image 407,544 B.
The only field that moved is the version stamp, 0.3.1 to 0.4.0.

**The v0.3.1 stamp was stale, and the code was already current.** The tell was
there before the flash and was missed: the MCUboot header reported 407,544 B and
the fresh v0.4.0 build linked to **407,544 B**, byte for byte. A real version
gap across 599 commits does not produce an identical image size.

Two consequences, both worth writing down:

1. **This is not a version comparison.** It is a like-for-like re-capture that
   confirms the baseline reproduces across a reflash and a reset. Treat §1 and
   §1.1 as one measurement taken twice, not two data points.
2. **The pairing survived, and the CHANGELOG's warning did not apply here.**
   [`CHANGELOG.md`](../CHANGELOG.md) `[0.4.0]` says existing pairings do not
   survive, because the settings partition moves from `0x7e000` to `0x7c000` and
   the Matter record schema becomes `mf2`. That describes a genuine v0.3.0
   board. This one was already on the new layout, so there was nothing to
   migrate. Measured directly: the settings partition held 10,979 non-`0xFF`
   bytes before the flash and 11,464 after, with 485 bytes differing, which is
   ordinary NVS append on boot. The node rejoined the same Thread network on the
   same channel (2475 MHz, channel 25).

**Take the backup anyway.** It cost one SWD read and it was the only thing that
made the "did the pairing survive?" question answerable afterwards rather than
a guess:

```sh
probe-rs read b8 0x0007C000 16384 --output settings.bin --format binary
```

Check the result is 16,384 bytes and not all `0xFF` before trusting it. An
erased page saves silently and looks exactly like a backup.

---

## 2. What each line settles

| Question | Register | Verdict |
| --- | --- | --- |
| Is the LFXO used, or the RC oscillator? | `LFCLKSTAT = 0x00010001` | **LFXO, running.** MEASURED. Nothing to gain. |
| Is the DC/DC converter enabled? | `POWER.DCDCEN = 1` | **Enabled.** MEASURED. Nothing to gain, and the module does fit the inductor. |
| Is REG0 (high voltage) left in LDO mode? | `POWER.DCDCEN0 = 0` | Off, which is the topology (VDDH tied to VDD, REG0 bypassed), not a finding. |
| Is the CPU stuck awake? | `RTC1` advancing, no `CONSTLAT` anywhere in tree | Kernel running normally, System ON sleep reachable. |
| Is the DW3110's SPI left enabled? | `SPIM3.ENABLE = 0` | Disabled between transfers. Correct. |
| Is USB drawing? | `USBD.ENABLE = 0` | Off. `usb_enable()` is provisioning-only (`main.c:239`). Correct. |
| Is anything advertising-adjacent left on? | `SAADC`, `UARTE1`, `SPIM2`, `TWI/SPI1` all 0 | Clean. |
| **Is the 2.4 GHz radio idle?** | `MODE = 0x0F`, `STATE = 3` on **20/20** | **No. 802.15.4 receive, 100% duty.** |
| **Is UART0 enabled with nobody using it?** | `UART0.ENABLE = 4` | Enabled, legacy UART mode. Idle almost always, but it is the MCUmgr serial-DFU transport, not a leak. See §2.2. |

### 2.1 The radio result is the Thread MED decision, showing up as current

`apps/dwm3001cdk-lock/overlay-thread.conf:52` chose MED over SED deliberately,
and said why in the file: an SED with `POLL_PERIOD=3000` was "the single largest
share of tile-tap latency", and

> Keeping the receiver on is pre-cleared on this exact part [...] The board is
> USB-powered, so rx-on costs power nothing here pays for and buys latency.

That is exactly what the 20/20 RX samples are. The decision is coherent and
documented; this baseline just prices it. The same file already carries the
recipe for reversing it if a battery ever appears, and it is more specific than
any outside advice on the subject: `CONFIG_OPENTHREAD_MTD_SED=y` with
`CONFIG_OPENTHREAD_POLL_PERIOD=500`, **and** move the advertised SII in
`src/matter_thread_port.c` and `modules/ultrawidelock_matter/src/matter_case.c`
with it, "the three lie if they diverge".

### 2.2 UART0 is enabled and idle, but it is a transport, not a leak

**This section said "the one unambiguous leak" and that was wrong for the
shipping image.** The correction is worth more than the original claim.

`ENABLE = 4` is "enabled, legacy UART", not `8` (UARTE) and not `0`. The console
is RTT (`CONFIG_UART_CONSOLE=n`) and the shell is bound to CDC-ACM, so at a
glance nothing in the application uses it and
`&uart0 { status = "disabled"; };` looks like a free win.

**It is not free.** `apps/dwm3001cdk-lock/overlay-smp.conf:149` sets
`CONFIG_MCUMGR_TRANSPORT_UART=y`, and MCUmgr binds to the `zephyr,uart-mcumgr`
chosen node, which the upstream board DTS points at `&uart0`. That is the
signed-update-over-a-cable path the browser flasher uses
(`web/flasher/serial.js`). Disabling uart0 in an `SMP=1` build does not remove a
leak, it removes a shipped transport.

So the honest reading of `ENABLE = 4` is: a transport that is enabled and idle
almost all of the time, waiting for a DFU session that happens rarely. The way
to have both is `CONFIG_PM_DEVICE` with runtime suspend on uart0, so the
peripheral is only powered while a transfer is in flight. That is a real
follow-up and is not attempted here.

Two caveats on the size of the prize, both of which argue for leaving it alone
until the radio is dealt with:

1. An nRF52 legacy UART is the classic HFCLK pin, but HFXO is already held here
   by the permanent 802.15.4 receive, so **UART0's marginal cost is masked
   today**. It only becomes visible once the radio duty drops.
2. A build that genuinely does not need serial DFU can already drop the whole
   thing: `apps/dwm3001cdk-lock/overlay-anchorlink.conf:40` sets
   `CONFIG_SERIAL=n` and proves the app links without it. MCUboot's serial
   recovery is a separate image and is unaffected either way.

---

## 3. The ranking (states MEASURED, milliamps PREDICTED)

DW3110 figures from the [DW3000 Datasheet](https://www.mouser.com/pdfDocs/DW3000DataSheet5.pdf)
v1.3, Figures 26 and 28, all supplies at 3.0 V, channel 5, 6.8 Mbps. nRF52833
figures from the [nRF52833 Product Specification](https://media.distrelec.com/Web/Downloads/_m/an/NRF52833-QIAA-R7_eng_man.pdf)
v1.6 §6.18.16, DC/DC at 3 V.

| Term | Measured state | Published current | Duty | Contribution |
| --- | --- | --- | --- | --- |
| **DW3110 idle** | never slept; `dwt_entersleep()` called nowhere | `IDLE_PLL` **18 mA**, `IDLE_RC` **8 mA** | 100% | **8 to 18 mA** |
| **nRF52833 802.15.4 RX** | `MODE=0x0F`, `STATE=3`, 20/20 | see note below | 100% | **~4.6 to 6.5 mA** |
| nRF52833 UART0 | `ENABLE=4`, unused | not published separately | 100% | sub-mA, masked today |
| nRF52833 BLE advertising | `BT_LE_ADV_CONN_FAST_1`, 30 to 60 ms | `ITX,0dBM,DCDC` 4.9 mA, `IRX,1M,DCDC` 4.6 mA | ~2 to 5% | ~0.1 to 0.3 mA |
| CPU housekeeping tick | 250 ms forever (`main.c:111`) | | 4 wakes/s | tens of µA |
| Log wrap timer | 16 s forever (`ultrawidelock_logfmt.c:257`) | | 0.06 wakes/s | negligible |
| TWIM0 (accelerometer bus) | `ENABLE=6` | | idle | µA |
| CPU in System ON sleep | LFXO on, DC/DC on | | remainder | µA |

**Board total, the two ICs only: order 13 to 25 mA continuous, with the DW3110
the largest single term.** This excludes the J-Link OB and the LEDs, which share
the USB rail and cannot be separated without the current-measurement header.

Two honest gaps in that table:

1. **The nRF52833 PS publishes no IEEE 802.15.4 RX current row.** The nearest
   published figures are `IRX,1M,DCDC` = **4.6 mA** and `IRX,2M,DCDC` = 5.2 mA.
   802.15.4 receive is not one of them and is generally quoted higher on this
   family. Treat 4.6 mA as a published floor, not the answer.
2. **Whether the DW3110 rests in `IDLE_PLL` (18 mA) or `IDLE_RC` (8 mA) is not
   measured.** `dwt_forcetrxoff()` returns the part to `IDLE_PLL` per the
   datasheet's state diagram, so 18 mA is the likely case, but the deciding
   register is `SYS_STATE_LO` on the DW3110 and it is only reachable over SPI
   from firmware, not over SWD. §5.2.

---

## 4. What the baseline changes about the optimization list

**The DW3110 is the whole game, and by a wider margin than expected.**
`IDLE_PLL` is **18 mA** and `DEEPSLEEP` is **260 nA**: a factor of about 69,000.
Sleeping it removes roughly three quarters of the resting current of the two
ICs, and it is the only item on any list that costs nothing in latency or
function:

- Sleep parameters are already configured. `uwb_min.c:155` calls
  `dwt_configuresleep(DWT_CONFIG | DWT_GOTOIDLE | DWT_RUNSAR, DWT_WAKE_CSN | DWT_SLP_EN)`,
  so wake-on-CS and config restore are armed.
- The wake path is already written, registered and datasheet-correct.
  `ports/zephyr/dw3000/dw3000_hw.c:268` drives CS low about 500 µs then waits
  2 ms for IDLE_RC; the datasheet's warm-start timing (Figure 22) asks for CS
  held **at least 500 µs** and about **1 ms** to IDLE_RC.
  `modules/ultrawidelock_dw3000/src/deca_port.c:57` binds it as
  `.wakeup_device_with_io`.
- Only the sleep-entry half is missing. It is named in a comment and never
  written: `qpwr_uwb_sleep()`, `modules/ultrawidelock_dw3000/include/dw3000_hw.h:17`.
- **The wake is free in wall-clock terms.** Ranging starts only after BLE auth
  completes (`reader.c:1212`, gated again on the RSSI gate), which is tens of ms.
  A 1 to 2 ms wake hides entirely inside that.

One integration risk to check before landing it: `ultrawidelock_uwb_prewarm()`
(`ultrawidelock_uwb_facade.c:118`) pre-applies the session PHY so the M4-time
start can skip `dwt_configure`. Confirm that survives the AON restore, or sleep
only before prewarm rather than after.

Second: **the Thread MED rx-on is the number two term and no outside list
identified it.** It is a deliberate, documented latency purchase, not an
oversight. It only becomes wrong if the power budget changes.

Third, and this is the finding that ranks everything else: **there is no power
budget.** `overlay-thread.conf:60` and `docs/inside-latch.md:211` both state the
board is USB powered, `docs/bench-inside-outside.md:251` lists power as an
explicit non-goal, and `PRODUCT.md` names no battery target. Every item except
the DW3110 sleep trades away latency or range-integrity margin that this repo
bought with measured work. Those trades cannot be priced without a cell
chemistry, a capacity in mAh, a target service life and an unlocks-per-day rate.

---

## 5. Turning the PREDICTED column into MEASURED

### 5.1 The real current measurement, when a PPK2 exists

Not run. Nothing on this machine can measure current: the USB enumeration showed
one SEGGER J-Link and no Nordic vendor ID.

1. Cut the debugger and LED rails, or the J-Link OB swamps everything. See
   [`dwm3001cdk-surgery.md`](dwm3001cdk-surgery.md) for what is safe to lift on
   this board.
2. PPK2 in source-meter mode across the board's current-measurement header, 3.0 V
   to match the datasheet conditions above.
3. Take four traces, each against the same firmware, changing one lever at a
   time so the arithmetic in §3 is attributable:
   - as-is (this baseline)
   - `qpwr_uwb_sleep()` landed
   - plus `&uart0 { status = "disabled"; };`
   - plus `CONFIG_OPENTHREAD_MTD_SED=y` / `POLL_PERIOD=500` and the SII moved
4. Record each as a row here with the date and the image version, the way
   `overlay-thread.conf` records its flash and RAM numbers.

### 5.2 The DW3110 resting state, without a meter

`SYS_STATE_LO` says whether the part rests in `IDLE_PLL` or `IDLE_RC`, and that
is 10 mA of the estimate in §3. It is a SPI register, so SWD cannot reach it. The
cheap way is a shell command that reads it on demand, added next to the existing
provisioning shell (`apps/dwm3001cdk-lock/src/prov_shell.c`,
`ports/zephyr/shell/ultrawidelock_shell.c`), read over the CDC-ACM console.

### 5.3 What the script cannot see

CPU idle residency. Reading the PC requires halting the core, which perturbs
exactly what is being measured. It is left out deliberately: with the DW3110 at
8 to 18 mA and the 802.15.4 receiver at about 5 mA, the CPU's sleep-versus-active
delta is one to two orders of magnitude down and cannot change the ranking. It
becomes worth measuring only after both radios are dealt with, at which point
`CONFIG_THREAD_RUNTIME_STATS` is the cheaper instrument than PC sampling.

---

## 6. What was implemented

### 6.1 The DW3110 sleeps now

`CONFIG_ULTRAWIDELOCK_UWB_DEEPSLEEP`, default y, Zephyr only. Four files:

| File | Change |
| --- | --- |
| `modules/ultrawidelock_uwb/Kconfig` | the option, with the 18 mA / 260 nA figures in its help |
| `modules/ultrawidelock_uwb/include/uwb_min.h` | declares `uwb_min_sleep()` |
| `modules/ultrawidelock_uwb/src/driver/uwb_min.c` | the sleep entry, and the wake in `uwb_radio_ensure_init()` |
| `modules/ultrawidelock_uwb/src/ccc/ccc_shim_rx.c` | calls it from `ccc_prepoll_stop()` |

Three things decided the shape:

1. **`DWT_DW_IDLE_RC`, not `DWT_DW_IDLE`.** The two halves have to agree on
   which state the part wakes into. `dw3000_hw_wakeup()` spins on
   `dwt_checkidlerc()`, so sleeping with `DWT_DW_IDLE` would wake into
   `IDLE_PLL` and leave that spin running to its 5 ms timeout every session.
   `IDLE_RC` is also what the SDK recommends for wake speed.
2. **The wake goes before the `g_radio_ready` early return**, not after. That
   fast path is exactly the one a second session takes, and a slept part
   answers SPI with nothing. Every route into the radio funnels through
   `uwb_radio_ensure_init()`, which is why `uwb_min_sleep()` deliberately has
   no public counterpart: waking is not a decision a caller is trusted to
   remember. `dw3000_hw_wakeup()` already no-ops when the part is awake.
3. **`ccc_prepoll_stop()` sleeps whether or not a listener was up.** Its old
   early return skipped the SPI work when the driver might be unprobed, and
   that was the case that mattered most: `ultrawidelock_ranging_init()` probes at boot
   and stops without ever listening, so a board no phone had come near still
   sat in `IDLE_PLL` from power-on. The no-SPI-when-unprobed guarantee moved
   into `uwb_min_sleep()`, which makes the same `g_radio_ready` test.

**The prewarm risk named in §4 turned out not to exist.** `ccc_prepoll_stop()`
already sets `g_phy_valid = false` on every stop, so the next prewarm re-runs
`dwt_configure` regardless of what the AON restored. Nothing depends on the PHY
surviving the sleep.

### 6.2 What was deliberately not done

**UART0.** It is a transport, not a leak. See §2.2.

**The 250 ms housekeeping tick and the 16 s log wrap timer.** §3 ranks these at
tens of µA and negligible. Against a post-sleep board total on the order of
5 mA they are under 1% between them, and the tick change would alter timing on
the unlock path while the log-timer change needs the DWT timestamp source
reworked to be wrap-free. Neither is worth its risk at that size. If a PPK2
later says §3 was wrong about them, they are easy and the analysis is in place.

**Two-rate BLE advertising.** Needs a design, not a constant: a flat slower
interval regresses walk-up latency, Matter commissioning discovery and the Web
Bluetooth chooser at once. Not attempted here.

### 6.3 The largest remaining term is a decision, not a bug

With the DW3110 asleep, the **802.15.4 receiver at 100% duty is what is left**,
and on the numbers in §3 it is roughly 90% of the remainder. It is not an
oversight: `overlay-thread.conf:52` chose MED over SED deliberately, measured
the latency it bought, and wrote down the exact recipe for reversing it. That
recipe is unchanged and is the next lever, but it is a product decision about
walk-up and tile-tap latency, so it is not taken here.

### 6.4 What is verified, and what is not

**VERIFIED on this silicon:** the sleep entry runs at boot and the board carries
on. RTT from the flashed image, `CONFIG_ULTRAWIDELOCK_UWB_DEEPSLEEP=y`:

```
I: DW3000 raw DEV_ID = 0xdeca0302 (expect 0xDECA03xx)
DIAG ull_configure chan=9
DIAG ull_setchannel ch=9 dw_state=0x03
DIAG ioctl ENTERSLEEP parm=2
  ⟐ idle · no ranging · sts○
```

`ENTERSLEEP parm=2` is the SDK's ioctl dispatch showing `DWT_ENTERSLEEP` with
`DWT_DW_IDLE_RC`. That is the boot-time probe-and-stop path from §6.1 item 3
putting the part down, which is the case that used to leave a board at 18 mA
from power-on. No fault, no `WAKEUP: chip never reached IDLE_RC`, and the node
went on to rejoin Thread (§1.1 registers, re-read after the flash, unchanged).

**MEASURED:** the host suite runs 9,610 checks with 0 failures, two of them new
and asserting exactly this: that a cold stop still sleeps, and that a stop after
a listen sleeps *after* the `forcetrxoff` rather than instead of it.

**VERIFIED end to end: a real iPhone walk-up unlocked the lock**, on a board that
had slept the DW3110 at boot. From the RTT ring immediately afterwards:

```
DIAG ull_setchannel ch=9 dw_state=0x03
I: credential session created
I: Sending RangingSessionSetupM1 message
I: Message RangingSessionSetupM2 received
I: slot bm peer=0x7c ours=0xff common=0x7c -> bit2 chaps=6 dur=2400 rstu
I: Sending RangingSessionSetupM3 message
I: Message RangingSessionSetupM4 received
I: credential start: sid=0x7575344b ch=9 code=9 slot=2400 blk=192ms spr=12
I: Pre-POLL accepted: URSK proven on air (sts0 305935ea)
  ⟐ rx ✓90 ✗22 ⧗0 tx18 · sts●
```

That is the whole path: the part woke, the PHY was re-applied, the M1 to M4
setup exchange completed, and the STS was proven on air across 90 good
receptions. **A chip still in DEEPSLEEP cannot produce any of those lines**, so
this is the wake working rather than an inference about it.

`dw3000_hw_wakeup()`'s own `WAKEUP CS` line had already scrolled out of the 1 KB
release RTT ring by the time it was read, because a session is far more verbose
than the ring is deep. It is not needed: `Pre-POLL accepted` is the stronger
statement, being the radio doing cryptographic work on air rather than a
host-side print.

So both halves are proven on this silicon. The switch stays because of what one
walk-up does **not** sample: a second session after a first, a long idle before
the first, a cold board, and the two ports this does not cover. If a walk-up
ever does regress, `make monitor` names it directly, and the fix is
`CONFIG_ULTRAWIDELOCK_UWB_DEEPSLEEP=n`:

| Line | Means |
| --- | --- |
| `WAKEUP: chip never reached IDLE_RC` | the wake failed its bounded `dwt_checkidlerc()` spin |
| silence where ranging used to start | the part did not come back at all |

**Also not verified:** that the current actually dropped. That still needs the
PPK2 procedure in §5.1. The register baseline cannot see it, because the DW3110's
state lives behind SPI where SWD cannot reach (§5.2).
