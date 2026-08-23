# Porting to a new chipset

How the UWB engine moves to a new chipset, what it costs, and how to prove a port did not
change the code the validated target runs.

The primary target is the DWM3001CDK on NCS v3.3.0, built from
[`firmware/`](../apps/dwm3001cdk-lock): reader, Matter node and Thread MTD in one nRF52833 image,
hardware-validated to an approach unlock and a live Apple Home tile. The nRF5340 DK in
[`apps/nrf5340dk-lock/`](../apps/nrf5340dk-lock/) is the NFC target and the one this chapter's
regression check uses, because it carries the largest build; its Nordic-binary path is
hardware-validated end to end and the source-stack default has a dedicated firmware CI
build and protocol host tests and awaits the full phone checklist. A third port,
ESP32-S3 on ESP-IDF, lives in [`ports/esp32/`](../ports/esp32/).

This chapter covers the UWB/RTOS seam. A complete Matter/credential lock must also
meet the cross-port five-fabric, Thread-dataset, selective-removal, SRP, and
last-fabric cleanup rules in [`PORTING.md`](../PORTING.md#matterhome-key-multi-admin-contract).
Those are externally visible behavior contracts even when an ESP32 or nRF5340
delegates its implementation to CHIP.

## 1. The contract

Everything the ranging engine needs from a platform is the two headers in
[`modules/ultrawidelock_port/include/`](../modules/ultrawidelock_port/include/):

- **[`ultrawidelock_port.h`](../modules/ultrawidelock_port/include/ultrawidelock_port.h)** — eight functions plus one mutex:

  | Function | Meaning |
  |---|---|
  | `ultrawidelock_malloc` / `ultrawidelock_calloc` / `ultrawidelock_free` | heap |
  | `ultrawidelock_uptime_us` | monotonic microseconds since boot |
  | `ultrawidelock_uptime_ms` | monotonic milliseconds since boot |
  | `ultrawidelock_sleep_ms` | relinquish the CPU for at least N ms |
  | `ultrawidelock_sleep_us` | short busy-wait, microseconds (`deca_sleep`) |
  | `ultrawidelock_cycle_get_32` | free-running counter, RX-arm latency probe |
  | `ultrawidelock_mutex_init/lock/unlock` | blocking mutex (credential reader trust store; three lines per backend) |

- **[`ultrawidelock_log.h`](../modules/ultrawidelock_port/include/ultrawidelock_log.h)** — `LOG_ERR/WRN/INF/DBG`, the
  hexdump variants, `LOG_MODULE_REGISTER/DECLARE`, and `ultrawidelock_printf`.

Both select a backend from `__ZEPHYR__`, `ESP_PLATFORM`, or `ULTRAWIDELOCK_PORT_HOST`, and `#error`
if none is defined rather than guessing. Two further headers in
`modules/ultrawidelock_uwb/src/facade/`, `ultrawidelock_bytes.h` (endian-neutral load/store) and `ultrawidelock_util.h`
(`MIN`/`MAX`/`ARRAY_SIZE`/`IS_ENABLED`), are pure code with no platform content at all;
they are shared by every target including Zephyr.

**Deliberately not in the contract:** work queues, timers, and init hooks. Those appear only
in `uwb_rxdiag.c`, `uwb_selftest.c`, `ultrawidelock_logfmt.c`, `ultrawidelock_logquiet.c` and `ultrawidelock_shell.c`,
which are Zephyr-only by design and are in no port's source list. The `k_work` / `k_sem` /
`k_poll` surface used by `dw3000_spi.c` and `dw3000_hw.c` is likewise excluded, because every
port supplies its own backend for those two files. Adding any of it would multiply the port
surface for code that never runs on the ranging path.

## 2. What a port costs

The cost axis is the **RTOS**, not the chipset. Under Zephyr the SPI and GPIO layers are
devicetree-abstracted, so a new Zephyr-supported SoC needs no C at all.

| Tier | Work | Targets | Effort |
|---|---|---|---|
| **0. Board file** | Devicetree overlay, roughly 50 lines. No C. | nRF52840, nRF54L15, EFR32MG24, STM32WB55, any Zephyr SoC with SPI + GPIO IRQ | Hours |
| **1. New RTOS** | A branch in `ultrawidelock_port.h` + `ultrawidelock_log.h` (about 55 lines), plus a DW3000 SPI/GPIO/IRQ backend (about 350 lines) | ESP-IDF (**done**), Pico SDK (RP2350), STM32Cube + FreeRTOS, bare metal | 2 to 4 days |
| **2. New UWB silicon** | A new driver under the `ultrawidelock_uwb_facade.h` seam. Not a port. | Anything that is not DW3xxx | Weeks, gated on driver availability |

Effort figures other than ESP-IDF are estimates from the measured line counts, not from
completed ports.

**Scope of the Tier 0 claim.** It applies to the **UWB engine module**, which is
SoC-neutral apart from one guarded block: the nRF5340 HFCLK boost in `ultrawidelock_uwb_facade.c`.
It does **not** cover the full Matter door-lock product. `scripts/nrf5340dk-build.sh` pins
`nrf5340dk/nrf5340/cpuapp` and drives a sysbuild with a separate `ipc_radio` network-core
image, so moving the whole application to a single-core part such as nRF52840 is a
sysbuild and Matter-transport exercise well beyond a devicetree overlay. Porting the
engine to a new Zephyr board is cheap; porting the product is not, and the two should not
be quoted at the same price.

For reference, the ESP-IDF port's entire target-specific surface for the ranging engine
is `ports/esp32/components/ultrawidelock_uwb/port/`: `dw3000_spi.c` (169), `dw3000_hw.c` (180) and
`ultrawidelock_wrap_stubs.c` (21). It carries no Zephyr compatibility layer.

**Beyond the engine: the full reader.** The tiers above cover secure ranging. A complete
lock additionally needs the credential-auth reader from `modules/ultrawidelock_cred`, which brings
two more per-platform seams, both small and both with ESP-IDF worked examples:

- a **BLE transport** implementing [`ultrawidelock_ble.h`](../modules/ultrawidelock_cred/include/ultrawidelock_ble.h)
  (the NimBLE backend is `ports/esp32/components/ultrawidelock_ble/ultrawidelock_ble_esp32.c`);
- a **storage backend** for the `ultrawidelock_prov` trust store (the NVS one is
  `ports/esp32/components/ultrawidelock_reader/ultrawidelock_prov_nvs.c`).

Portable crypto calls `ultrawidelock_prim.h`. Target builds bind that contract
once to `ultrawidelock_prim_psa.c`, over the framework's PSA implementation;
host tests bind it to a deterministic double. Modules do not call the framework
provider directly.

## 3. Build tiers

- **Full Aliro UWB** — `CONFIG_ULTRAWIDELOCK_UWB=y`, `ULTRAWIDELOCK_UWB_RESPONDER=y`, `ULTRAWIDELOCK_CRED=y`.
- **Bring-up only** — `CONFIG_ULTRAWIDELOCK_UWB=y` alone compiles just `uwb_min.c`.
- **No UWB (NFC-only)** — `CONFIG_ULTRAWIDELOCK_UWB=n`. The whole module is wrapped in
  `if(CONFIG_ULTRAWIDELOCK_UWB)` and contributes nothing; every external call site in
  `integrations/nrfconnect-door-lock/patches/custom_impl-uwb.patch` is `#ifdef CONFIG_ULTRAWIDELOCK_CRED`, so the build
  links clean with no UWB silicon present.

  This matters more than it looks. Aliro makes NFC the mandatory transport and BLE and UWB
  optional, so an NFC-only lock is a legitimate certified device, and this tier removes the
  DWM3000EVB from the bill of materials. Note that it also uses none of the ranging engine:
  the NFC path needs the credential-auth layer, not `modules/ultrawidelock_uwb`.

### Crypto seam

`ccc_kdf.h` needs one AES-ECB primitive. `ccc_crypto_prim.c` is the thin adapter
from that existing CCC contract to `ultrawidelock_aes_ecb_encrypt()`, including
both 128-bit and 256-bit keys. Every target links exactly one primitive provider;
there is no UWB-specific crypto-backend selector. Zephyr uses nrf_security or
Mbed TLS PSA, and ESP-IDF and FreeRTOS use their Mbed TLS PSA providers.

### SoC-specific seams

`ultrawidelock_uwb_facade.c` boosts the nRF5340 app-core HFCLK to 128 MHz for the DW3000 SPI bus,
guarded by `CONFIG_SOC_NRF5340_CPUAPP`. Other SoCs clock their SPI controller independently,
so it compiles to a no-op. A new target needs an equivalent only if its SPI clock is
divided at boot.

### STS seam

The CCC STS substitution rides a compile-time seam, `modules/ultrawidelock_uwb/include/uwb_seam.h`.
Four decadriver entry points carry engine behaviour a caller must not skip, so every call
site in the module goes through a helper instead of `<deca_device_api.h>`:

| Helper | Supplied by | Replaces | Carries |
|---|---|---|---|
| `ultrawidelock_uwb_arm_rx` | `ccc_shim_rx.c` | `dwt_rxenable` | programs the CCC key/IV for the slot |
| `ultrawidelock_uwb_set_sts_iv` | `ccc_shim_wrap.c` | `dwt_configurestsiv` | substitutes the CCC STS-V per frame |
| `ultrawidelock_uwb_set_callbacks` | `uwb_rxdiag.c` | `dwt_setcallbacks` | inserts the Pre-POLL shim |
| `ultrawidelock_uwb_configure_phy` | `uwb_rxdiag.c` | `dwt_configure` | traces the PHY configuration |

Below the `CONFIG_ULTRAWIDELOCK_CRED` tier there is no engine to reach and each helper inlines to the
plain decadriver call. The ESP32 port omits `uwb_rxdiag.c`, which is `k_work`-based, and
supplies the last two from `port/ultrawidelock_seam_stubs.c`.

This replaced an earlier `-Wl,--wrap=dwt_*` link-time interposer, and the reason matters for a
port: the seam is now plain C, so it needs no linker feature at all and a non-GNU toolchain is
no longer a porting problem. What the linker used to guarantee structurally is now enforced by
`make seam` (the `uwb-seam` gate in `make check`), which scans the tracked
sources for a call that reaches past the seam and carries a `--self-test` proving it can fail.
That guarantee is worth keeping mechanical: a site that bypasses the seam is silent on the
bench, because the radio still arms and ranging still runs, and only the unlock never happens.

## 4. Verifying a port did not change the validated target

Porting work touches shared code, so the nRF5340 image must be shown to be unaffected
rather than assumed to be. The check that catches real regressions is per-object, because
whole-image numbers are dominated by the Matter application and will hide a small engine
change.

```sh
SIZE=<zephyr-sdk>/arm-zephyr-eabi/bin/arm-zephyr-eabi-size
D=build/nrf5340dk/matter-aliro-door-lock-app/modules/ultrawidelock_uwb/CMakeFiles/ultrawidelock_uwb.dir/src

make nrf-build                                    # before the change
find $D -name '*.obj' | sort | xargs $SIZE > /tmp/before.txt
# ... make the change ...
make nrf-build
find $D -name '*.obj' | sort | xargs $SIZE > /tmp/after.txt
diff /tmp/before.txt /tmp/after.txt                # must be empty for a pure refactor
```

Repeat for the vendored DW3000 objects under
`build/nrf5340dk/matter-aliro-door-lock-app/modules/ultrawidelock_dw3000/` if `deps/dw3000` was touched, and run
`make check` (every host suite, no toolchain or hardware needed).

A byte-identical size table proves codegen is unchanged; it does not prove the port works.
Only a bench run against a phone does that.

## 5. Vendored DW3000 and the `printk` alias

`deps/dw3000` is vendor source with one local addition: DIAG tracing this project added to
`deca_compat.c`, `deca_interface.c` and `dw3000_device.c` (about 18 `printk` call sites,
gated on `CONFIG_ULTRAWIDELOCK_PRETTY_SHELL`). To keep those files to a one-line include change rather
than rewriting vendor call sites, `ultrawidelock_log.h` aliases `printk` itself on non-Zephyr targets.
That alias exists only for this purpose and should be removed with the DIAG tracing.

`deca_port.c` is shared by all ports and uses `ultrawidelock_sleep_ms` / `ultrawidelock_sleep_us`;
`dw3000_spi.c`, `dw3000_hw.c` and `dw3000_spi_trace.c` are Zephyr-specific and each port
replaces them.

## 6. Known-blocked targets

Blocked on driver availability, not on engineering:

- **NXP SR150** — NDA/production access only.
- **QM33 / QM35 as UWB silicon** — the public `qm35-sdk` on GitLab is clonable without an
  NDA, but it ships FiRa ranging and 360° AoA and does not mention Aliro. The Nordic and
  Qorvo Aliro reference application is distributed to "early technology adopters" on
  request; the terms are not public. Whether the public SDK exposes the STS key injection
  this engine needs is unverified. Its licence agreement was not read and may restrict
  redistribution.
