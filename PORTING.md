# Porting UltraWideLock

This guide is the shortest path to a new board or chipset. It assumes the board
uses Zephyr or ESP-IDF. A new operating system also needs the contracts listed
under [New operating systems](#new-operating-systems).

## Choose the integration path

| Target | Start from | Keep |
|---|---|---|
| Zephyr reader | `apps/dwm3001cdk-lock/` or `apps/nrf5340dk-lock/` | Module selection and `ports/zephyr/` |
| Zephyr initiator | `examples/zephyr/nrf5340dk-initiator/` | Device role manifests and `ports/zephyr/` |
| ESP-IDF reader | `examples/esp32/reader/` | `EXTRA_COMPONENT_DIRS` and `ultrawidelock_reader` requirements |
| ESP-IDF initiator | `examples/esp32/initiator/` | `EXTRA_COMPONENT_DIRS` and `ultrawidelock_device` requirements |

Keep the new application small. Board pins, devicetree, partitions, and feature
selection belong in the application. Portable protocol decisions belong in
`modules/`.

## C include surfaces

Application code includes only the stable role API it consumes:

```c
#include <ultrawidelock/reader.h>
#include <ultrawidelock/uwb.h>
```

An initiator uses `<ultrawidelock/device.h>`. A host tool that only handles protocol
data uses `<ultrawidelock/tlv.h>`. The all-in-one `<ultrawidelock/ultrawidelock.h>` is an
installed-package convenience and should not be used to pull unused firmware
roles into a target build.

Port implementations include the complete chipset contract:

```c
#include <ultrawidelock/ultrawidelock_hal.h>
```

In framework builds the role headers are available in the same `ultrawidelock/`
namespace. Their canonical declarations live under the owning module's
`include/ultrawidelock/` directory. Use that spelling in applications, modules,
ports, and tests; the removed flat role-header names are rejected by the SDK
gate.

## Five chipset seams

| Seam | Contract header | Existing backends |
|---|---|---|
| DW3000 GPIO, reset, and IRQ | `dw3000_hw.h` | `ports/zephyr/dw3000/`, `ports/esp32/components/ultrawidelock_uwb/port/` |
| DW3000 SPI | `dw3000_spi.h` | `ports/zephyr/dw3000/`, `ports/esp32/components/ultrawidelock_uwb/port/` |
| Reader BLE GATT and L2CAP | `ultrawidelock_ble.h` | `ports/zephyr/ble/`, `ports/esp32/components/ultrawidelock_ble/` |
| Initiator BLE central | `ultrawidelock_ble_central.h` | `ports/zephyr/ble/`, `ports/esp32/components/ultrawidelock_ble_central/` |
| Reader credential store | `ultrawidelock_prov.h` | `ports/zephyr/store/`, `ports/esp32/components/ultrawidelock_reader/` |

Implement every function in `dw3000_hw.h`, `dw3000_spi.h`, and `ultrawidelock_ble.h`
for a reader. Implement `ultrawidelock_ble_central_start()` and
`ultrawidelock_ble_central_send()` for an initiator. The parser and salt helpers in the
same header are portable code and must not be copied into a port. Implement only
`ultrawidelock_prov_load()`, `ultrawidelock_prov_store()`, and `ultrawidelock_prov_erase()` from
`ultrawidelock_prov.h`; serialization and trust policy remain portable.

If an existing backend already works for the chipset, do not fork it. Supply
pins and bus instances through the framework's board configuration. Add a new
backend file only when the hardware API is genuinely different.

## Persistent storage names

Each port names its own persistent records, and each framework caps those names
differently. The caps are not advisory: ESP-IDF's NVS silently reports "never
stored" when a read-only open names something too long, and only the write side
says so out loud, which is how an 18-character namespace survived a rename, a
test suite and a release before a bench session found it (docs/esp32-gotchas.md
§8.4).

This table is the source of truth for every declared record name. The purity gate
reads it: a name here that outgrows its port's cap fails, a name here that no
longer appears in the file named fails, and a storage call site in a file this
table does not list fails. Adding a record means adding a row. Key names written
inline at the call site rather than declared are not listed; the same gate
length-checks those where they are written.

<!-- storage-names:begin -->

| Port | Kind | Name | Cap | Declared in |
|---|---|---|---|---|
| esp32 | namespace | `uwl_prov` | 15 | `ports/esp32/components/ultrawidelock_reader/ultrawidelock_prov_nvs.c` |
| esp32 | key | `blob` | 15 | `ports/esp32/components/ultrawidelock_reader/ultrawidelock_prov_nvs.c` |
| esp32 | namespace | `presence` | 15 | `ports/esp32/components/ultrawidelock_reader/presence_link.c` |
| esp32 | key | `kdev` | 15 | `ports/esp32/components/ultrawidelock_reader/presence_link.c` |
| esp32 | namespace | `piv` | 15 | `ports/esp32/components/piv_ccid/piv_identity.c` |
| esp32 | key | `auth9a` | 15 | `ports/esp32/components/piv_ccid/piv_identity.c` |
| esp32 | key | `key9d` | 15 | `ports/esp32/components/piv_ccid/piv_identity.c` |
| esp32 | namespace | `ha_mqtt` | 15 | `apps/esp32-matter-lock/main/ha_mqtt.c` |
| esp32 | namespace | `satlink` | 15 | `ports/esp32/components/ultrawidelock_satlink/ultrawidelock_satlink.c` |
| esp32 | key | `lk` | 15 | `ports/esp32/components/ultrawidelock_satlink/ultrawidelock_satlink.c` |
| zephyr | subtree | `ultrawidelock` | 64 | `ports/zephyr/store/ultrawidelock_prov_settings.c` |
| zephyr | key | `ultrawidelock/prov` | 64 | `ports/zephyr/store/ultrawidelock_prov_settings.c` |
| zephyr | subtree | `mfab` | 64 | `ports/zephyr/store/matter_fab_settings.c` |
| zephyr | subtree | `msub` | 64 | `apps/dwm3001cdk-lock/src/matter_commission.c` |
| zephyr | key | `srp/hid` | 64 | `ports/zephyr/matter/matter_thread_port.c` |
| zephyr | subtree | `uwl/latch` | 64 | `apps/dwm3001cdk-lock/src/main.c` |
| zephyr | key | `uwl/latch/rec` | 64 | `apps/dwm3001cdk-lock/src/main.c` |
| zephyr | subtree | `uwl/wit` | 64 | `apps/dwm3001cdk-lock/src/witness_link.c` |
| zephyr | key | `uwl/wit/k` | 64 | `apps/dwm3001cdk-lock/src/prov_shell.c` |
| zephyr | subtree | `uwl/anc` | 64 | `apps/dwm3001cdk-lock/src/witness_link.c` |
| zephyr | key | `uwl/anc/k` | 64 | `apps/dwm3001cdk-lock/src/prov_shell.c` |
| zephyr | subtree | `wit` | 64 | `examples/zephyr/ble-witness/src/main.c` |
| zephyr | key | `wit/role` | 64 | `examples/zephyr/ble-witness/src/main.c` |
| zephyr | key | `wit/lk` | 64 | `examples/zephyr/ble-witness/src/main.c` |
| zephyr | key | `wit/gk` | 64 | `examples/zephyr/ble-witness/src/main.c` |
| zephyr | key | `wit/ds` | 64 | `examples/zephyr/ble-witness/src/main.c` |
| zephyr | subtree | `sat` | 64 | `apps/satellite/src/anchor_link.c` |
| zephyr | key | `sat/lk` | 64 | `apps/satellite/src/anchor_link.c` |
| zephyr | subtree | `uwl` | 64 | `ports/zephyr/store/kv_zephyr.c` |

<!-- storage-names:end -->

One row above is a subtree with no keys listed under it, deliberately. The
`uwl` subtree belongs to `ultrawidelock_kv.h`, which addresses records by a
`uint16_t` rather than by a name and lets each backend derive the storage name
from the number — `uwl/%04x` under Zephyr settings, namespace `uwl` with key
`%04x` under NVS. Eight characters and four, for every key in the 16-bit space,
so neither cap can be reached by any caller and there is nothing per-record to
list here or to keep in step. A record added through that seam takes an id from
a window in `ultrawidelock_kv.h`; it does not take a row in this table.

The rows above it are the older spelling, where each record names itself. Those
call sites have not moved onto the seam, and moving them is a separate decision:
the names hold provisioned data, and derived names are not the names already on
the flash.

Where the caps come from, and why they differ:

- **esp32** — `NVS_NS_NAME_MAX_SIZE - 1` and `NVS_KEY_NAME_MAX_SIZE - 1`, both 15,
  from ESP-IDF's `nvs.h`. Each name is also `_Static_assert`ed against the cap
  where it is defined, so a bench build fails before the flash does.
- **zephyr** — `SETTINGS_MAX_NAME_LEN`, `8 * SETTINGS_MAX_DIR_DEPTH` = 64, from
  `zephyr/include/zephyr/settings/settings.h`, with at most 8 `/`-separated
  levels. Roomy enough that no name here is close to it.
- **freertos-nrf52833** — no row, because that port has no name to cap. Records
  are numeric ids (`ULTRAWIDELOCK_KV_KEY_CRED_PROV` = `0x0001u`) in windowed
  ranges declared in `ports/freertos-nrf52833/include/ultrawidelock_freertos_kv.h`.
  A new record there takes an id from the right window, not a string.

The three ports share `ultrawidelock_prov.h` and the portable serializer, not
these names. A single spelling across all three is neither possible nor wanted;
what has to hold is that each name is declared once, listed once, and gated.

## Build integration

Zephyr applications add the required `modules/<name>` directories and
`ports/zephyr/` to `ZEPHYR_EXTRA_MODULES`. The existing applications show the
smallest known-good module set for each role.

ESP-IDF applications add the component root once:

```cmake
set(EXTRA_COMPONENT_DIRS "${ULTRAWIDELOCK_ROOT}/ports/esp32/components")
```

Then name the role in the consuming component's `REQUIRES` or `PRIV_REQUIRES`.
Use `ultrawidelock_reader` for a reader and `ultrawidelock_device` plus `ultrawidelock_ble_central` for
an initiator. ESP-IDF discovers every component but compiles only required
components, so an unreferenced backend is not verified.

Shared source selection comes from `modules/*/roles/*.list`. Add a source to one
role manifest and consume that role through the existing CMake helper. Do not
paste the source path into a second build definition.

## New UWB chipsets

The two `dw3000_*` seams above cover a new board carrying a DW3000-family
chip. A different chipset replaces the engine, not the seams. The contract is
`<ultrawidelock/uwb.h>`: bind a URSK, start and stop a credential session from the
negotiated parameters, report ranges with integrity evidence. Everything above
that header is chip-agnostic and reused as is — the FiRa session state, DS-TWR
math, CCC key schedule and MAC framing, the credential M1-M4 adapter, and the apps.

What a new chipset supplies:

1. An implementation of every function in `<ultrawidelock/uwb.h>`, in its own
   module directory beside `modules/ultrawidelock_dw3000/`.
2. Role manifests for its source sets, replacing the DW3000-shaped roles
   (`base_driver`, `ccc_engine`, `responder_driver`, `diag_cir`,
   `flight_recorder`); the chip-agnostic roles are consumed unchanged.
3. Wiring seams in `ports/` only if the chip is a raw transceiver. A chip that
   runs its own FiRa stack and speaks UCI needs no local ranging engine at
   all: the contract implementation translates sessions to UCI commands.

`tests/tooling/uwb_engine_scope_check.sh` (`make scope`) enforces the
boundary: the Qorvo radio API is named only inside the DW3000 engine's file
set, so chip-agnostic code cannot silently couple to one vendor's silicon.
Keep a new chipset's radio API inside its own engine the same way.

## New operating systems

The five HAL seams are not the complete contract for a new operating system.
Before chipset work, implement the platform services declared by:

- `ultrawidelock_osal.h`
- `ultrawidelock_flash.h`
- `ultrawidelock_log.h`
- `ultrawidelock_port.h`

Put that backend under one new `ports/<os>/` tree. Keep conditional operating
system code out of `modules/`. Add a host compile or fake for each new contract
before relying on a hardware build.

## Verification

Run the boundary gates before a target build:

```sh
bash tests/tooling/port_purity_check.sh --self-test
make check
```

Then build the closest existing role and the new target. For ESP-IDF also run
`bash tests/ports/esp32/verify_port.sh`. A new gate rule is complete only after
a temporary violation proves it fails and restoring the valid tree proves it
passes.
