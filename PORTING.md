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
| zephyr | subtree | `ultrawidelock` | 64 | `ports/zephyr/store/ultrawidelock_prov_settings.c` |
| zephyr | key | `ultrawidelock/prov` | 64 | `ports/zephyr/store/ultrawidelock_prov_settings.c` |
| zephyr | subtree | `mf2` | 64 | `ports/zephyr/store/matter_fab_settings.c` |
| zephyr | subtree | `msub` | 64 | `apps/dwm3001cdk-lock/src/matter_commission.c` |
| zephyr | key | `srp/hid` | 64 | `ports/zephyr/matter/matter_thread_port.c` |

<!-- storage-names:end -->

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

## Matter/Home Key multi-admin contract

Every Matter/Home Key lock port must present the same controller-visible behavior,
whether it uses the portable Matter implementation or an SDK-owned CHIP stack:

1. Hold at least five fabrics. A commissioning attempt owns provisional slots
   until CommissioningComplete is durably stored; fail-safe expiry removes only
   that attempt and never an established Apple Home or Home Assistant fabric.
   Exactly replay bounded state-changing responses when MRP retries a request.
2. Treat the established Thread dataset as node state, not administrator state.
   A later administrator may name the same Extended PAN ID without restarting
   Thread or replacing the committed credentials; a different or malformed
   dataset is refused without detaching the lock.
3. Scope ACL state, filtered fabric reads, subscriptions, ICAC ownership, and
   cleanup to the fabric that owns them. A removed or PASE session cannot
   operate the lock or administer another fabric.
4. Implement authenticated OperationalCredentials RemoveFabric. The removal
   is durably tombstoned before success, revokes that fabric's sessions and SRP
   service, and leaves every other fabric usable. Removing the last fabric also
   clears Home Key trust and reopens commissioning.
5. Preserve the SRP client key and host identity together. Service structures
   remain allocated until OpenThread returns them in its removal callback, and
   duplicate registration is retried without reporting false success.

The DWM3001CDK Zephyr and FreeRTOS builds share
`modules/ultrawidelock_matter/`, `ports/zephyr/matter/matter_thread_port.c`, and
the `mf2` record contract in `ports/zephyr/store/matter_fab_settings.c`. The
ESP32 and nRF5340 ports delegate fabric transactions to their CHIP SDKs; their
application glue still owns the last-fabric credential cleanup rule.

`mf2` is a deliberate clean break from the v0.3 custom schema. The loader never
trusts `mfab/*`, and the flash map moves the custom DWM settings region from
`0x7e000` to `0x7c000`. Upgrading those builds requires re-pairing Matter and
Home Key; when no current fabric survives, the application also clears the old
reader provisioning identity so a new home cannot inherit an old Wallet key.

| Port | Fabric owner | Contract status in this tree | Hardware status |
|---|---|---|---|
| DWM3001CDK Zephyr | portable stack + Zephyr `mf2` store | implemented and host-tested | production image builds and fits; Apple plus Home Assistant rows are still open |
| DWM3001CDK FreeRTOS | same portable stack + FreeRTOS `mf2` adapter | implemented and port-tested | Matter multi-admin not hardware-validated |
| ESP32 | esp-matter/CHIP | delegated to SDK; application handles last-fabric reader cleanup | existing ESP lock evidence does not cover this new cross-controller gate |
| nRF5340 | NCS/CHIP | delegated to SDK; integration must retain the same externally visible rules | existing Apple evidence does not cover Apple plus Home Assistant |

Operator setup and recovery for the primary target live in
[`apps/dwm3001cdk-lock/README.md`](apps/dwm3001cdk-lock/README.md#apple-home-plus-home-assistant).
Port parity means the behavior in this section, not a shared private flash
format across unrelated SDKs.

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
