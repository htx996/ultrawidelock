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
| esp32 | namespace | `presence` | 15 | `ports/esp32/components/ultrawidelock_reader/presence_link.c` |
| esp32 | key | `kdev` | 15 | `ports/esp32/components/ultrawidelock_reader/presence_link.c` |
| esp32 | namespace | `piv` | 15 | `ports/esp32/components/piv_ccid/piv_identity.c` |
| esp32 | key | `auth9a` | 15 | `ports/esp32/components/piv_ccid/piv_identity.c` |
| esp32 | key | `key9d` | 15 | `ports/esp32/components/piv_ccid/piv_identity.c` |
| esp32 | namespace | `ha_mqtt` | 15 | `apps/esp32-matter-lock/main/ha_mqtt.c` |
| esp32 | namespace | `satlink` | 15 | `ports/esp32/components/ultrawidelock_satlink/ultrawidelock_satlink.c` |
| esp32 | key | `lk` | 15 | `ports/esp32/components/ultrawidelock_satlink/ultrawidelock_satlink.c` |
| esp32 | key | `ch` | 15 | `ports/esp32/components/ultrawidelock_satlink/ultrawidelock_satlink.c` |
| esp32 | namespace | `satfuse` | 15 | `apps/esp32-matter-lock/main/sat_fusion.c` |
| esp32 | key | `bl1` | 15 | `apps/esp32-matter-lock/main/sat_fusion.c` |
| esp32 | key | `bl2` | 15 | `apps/esp32-matter-lock/main/sat_fusion.c` |
| esp32 | key | `bl3` | 15 | `apps/esp32-matter-lock/main/sat_fusion.c` |
| zephyr | subtree | `msub` | 64 | `apps/dwm3001cdk-lock/src/matter_commission.c` |
| zephyr | key | `srp/hid` | 64 | `ports/zephyr/matter/matter_thread_port.c` |
| zephyr | subtree | `uwl/latch` | 64 | `apps/dwm3001cdk-lock/src/main.c` |
| zephyr | key | `uwl/latch/rec` | 64 | `apps/dwm3001cdk-lock/src/main.c` |
| esp32 | namespace | `uwl` | 15 | `ports/esp32/components/ultrawidelock_port/kv_nvs.c` |
| zephyr | subtree | `uwl` | 64 | `ports/zephyr/store/kv_zephyr.c` |

<!-- storage-names:end -->

One row above is a subtree with no keys listed under it, deliberately. The
`uwl` subtree belongs to `ultrawidelock_kv.h`, which addresses records by a
`uint16_t` rather than by a name and lets each backend derive the storage name
from the number: `uwl/%04x` under Zephyr settings, namespace `uwl` with key
`%04x` under NVS. Eight characters and four, for every key in the 16-bit space,
so neither cap can be reached by any caller and there is nothing per-record to
list here or to keep in step. A record added through that seam takes an id from
a window in `ultrawidelock_kv.h`; it does not take a row in this table.

The rows above it are the older spelling, where each record names itself. They
are being retired one call site at a time. The credential provisioning blob went
first, on both ports that had a row for it: the Zephyr backend held the subtree
`ultrawidelock` and the key `ultrawidelock/prov`, the ESP32 backend held the
namespace `uwl_prov` and the key `blob`, and both now take
`ULTRAWIDELOCK_KV_KEY_CRED_PROV` from the seam, so neither has a row at all.
Each move costs the data already on the flash -- a derived name is not the name
a provisioned board wrote -- which is why they go one at a time and not at once.

Where the caps come from, and why they differ:

- **esp32**: `NVS_NS_NAME_MAX_SIZE - 1` and `NVS_KEY_NAME_MAX_SIZE - 1`, both 15,
  from ESP-IDF's `nvs.h`. Each name still in the table is also `_Static_assert`ed
  against the cap where it is defined, so a bench build fails before the flash
  does. A record on the seam needs no assertion: `%04x` is four characters
  whatever the caller passes.
- **zephyr**: `SETTINGS_MAX_NAME_LEN`, `8 * SETTINGS_MAX_DIR_DEPTH` = 64, from
  `zephyr/include/zephyr/settings/settings.h`, with at most 8 `/`-separated
  levels. Roomy enough that no name here is close to it.
- **freertos-nrf52833**: no row, because that port has no name to cap. Records
  are numeric ids (`ULTRAWIDELOCK_KV_KEY_CRED_PROV` = `0x0001u`) in the windows
  `modules/ultrawidelock_port/include/ultrawidelock_kv.h` reserves; this port
  reached that design first and the seam was derived from it.
  Matter's shared ids now live in `ultrawidelock_kv.h`; the port-specific
  `ports/freertos-nrf52833/include/ultrawidelock_freertos_kv.h` assigns only its
  PSA backend's ids. A new record takes an id from the right window, not a
  string.

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
that header is chip-agnostic and reused as is: the FiRa session state, DS-TWR
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

The five HAL seams are the chipset contract, not the operating system one. The
platform-service contracts live in `modules/ultrawidelock_port/include/`. The
crypto primitive contract lives with its callers in
`modules/ultrawidelock_cred/include/`. Together, this is all of them:

| Seam | What it covers | Backends today | Host fake |
|---|---|---|---|
| `ultrawidelock_port.h` | heap, uptime, sleep, cycle counter, blocking mutex, atomic exchange | in-header branches | the `ULTRAWIDELOCK_PORT_HOST` branch |
| `ultrawidelock_log.h` | `LOG_*` spellings and `ultrawidelock_printf` | in-header branches | the host branch, over `tests/host/logfake` |
| `ultrawidelock_bytes.h` | endian-neutral load/store | in-header: Zephyr defers to `<zephyr/sys/byteorder.h>`, everything else takes the portable inlines | the portable inlines |
| `ultrawidelock_osal.h` | work, delayed work, semaphore, thread | `ports/zephyr/osal/osal_zephyr.c`, `ports/esp32/components/ultrawidelock_port/osal_esp.c`, `ports/freertos-nrf52833/osal/osal_freertos.c` | `tests/host/port/osal_host.c` |
| `ultrawidelock_flash.h` | erase-block regions, for DFU | `ports/zephyr/osal/flash_zephyr.c`, `ports/freertos-nrf52833/storage/flash_area_freertos.c` | `tests/host/port/flash_host.c` |
| `ultrawidelock_kv.h` | small persistent values, addressed by number | `ports/zephyr/store/kv_zephyr.c`, `ports/esp32/components/ultrawidelock_port/kv_nvs.c`, `ports/freertos-nrf52833/storage/kv_flash_freertos.c` | `tests/host/port/kv_host.c` |
| `ultrawidelock_dgram.h` | the sealed link's datagrams | `ports/zephyr/net/dgram_openthread.c` | `tests/host/port/dgram_host.c` |
| `ultrawidelock_prim.h` | random bytes, AES ECB/GCM/CCM, P-256 | shared `ultrawidelock_prim_psa.c` over each framework's PSA provider | `tests/shared/ultrawidelock_prim_host.c`, with `tests/host/psafake` for the provider contract |

Three shapes appear in that table, and the difference is not cosmetic.

**In-header branches.** `ultrawidelock_port.h`, `_log.h` and `_bytes.h` are
`#if defined(__ZEPHYR__) / ESP_PLATFORM / ULTRAWIDELOCK_PORT_FREERTOS /
ULTRAWIDELOCK_PORT_HOST` inside the header itself. A new OS is a new branch
there. That suits contracts that are a handful of one-line inlines with no
state: a separate translation unit per port would cost more than it explains.

**A file per port.** `ultrawidelock_kv.h` and `ultrawidelock_dgram.h` have no
platform branches at all -- the header is contract only, and each port supplies
a `.c` under its own tree. This is the shape to prefer for anything with state,
a lifecycle, or error paths worth testing, and it is why those two are the only
seams whose host backend is a real fake a suite drives rather than a stub.
`ultrawidelock_osal.h` is a hybrid: the types branch in the header because they
wrap `k_work` and its equivalents, the functions are a file per port.

**A provider-backed adapter.** `ultrawidelock_prim.h` is a provider-neutral
contract. Zephyr, ESP-IDF and FreeRTOS builds use the shared PSA adapter, while
their framework supplies nrf_security or the Mbed TLS PSA implementation and
its lifecycle. Host tests supply a deterministic double. A framework without
PSA implements the same primitive contract in its own port tree; portable
modules still do not name that provider.

Whichever shape a new seam takes, two rules hold. Put operating-system-specific
code under one new `ports/<os>/` tree and keep conditional operating system code
out of `modules/`. A provider-neutral adapter may remain shared source.
And do not add a seam that has no second implementation: every one above has at
least two backends today, or one backend plus a host fake a suite actually
drives. A contract with a single implementation is a header, not a seam, and it
will be wrong in the ways only a second port would have revealed.

### A new framework, end to end

1. Implement the eight seams above. Nothing under `modules/` should need an
   edit; if it does, that is the bug to fix first.
2. Add the OS branch to the three in-header seams, one `.c` per port for the
   four service seams, and bind the primitive contract to one crypto provider.
3. Write one thin component wrapper that reads the role manifests
   (`modules/<mod>/roles/*.list`) rather than listing sources again. See
   `cmake/ultrawidelock_roles.cmake` and the ESP-IDF components for what that
   looks like.
4. Run `tests/tooling/port_purity_check.sh`. It fails on a source in two roles,
   a platform tree naming another OS, and a record stored under a name this file
   does not list.
5. Run `make check`. The host suite is the correctness story for every module a
   new port has not yet compiled.

## Matter/Home Key multi-admin contract

Every Matter/Home Key lock port must present the same controller-visible behavior,
whether it uses the portable Matter implementation or an SDK-owned CHIP stack:

1. Hold at least five fabrics. A commissioning attempt owns provisional slots
   until CommissioningComplete is durably stored; fail-safe expiry removes only
   that attempt and never an established Apple Home or Home Assistant fabric.
   Exactly replay bounded state-changing responses when MRP retries a request.
   That covers CommissioningComplete itself: a retransmission repeats the
   answer it already gave, never NO_FAIL_SAFE. Mutate the fabric table only
   under the lock that owns it, fail-safe expiry included, and never take that
   lock again from the store worker the caller is already waiting on.
2. Treat the established Thread dataset as node state, not administrator state.
   A later administrator may name the same Extended PAN ID without restarting
   Thread or replacing the committed credentials; a different or malformed
   dataset is refused without detaching the lock.
3. Scope ACL state, filtered fabric reads, subscriptions, ICAC ownership, and
   cleanup to the fabric that owns them. A removed or PASE session cannot
   operate the lock or administer another fabric. Parse stored ACL entries
   against the spec's field ids, pinned by a real controller's captured bytes,
   and match a subject in the CASE Authenticated Tag range as a tag under the
   version rule rather than by equality.
4. Implement authenticated OperationalCredentials RemoveFabric. The removal
   is durably tombstoned before success, revokes that fabric's sessions and SRP
   service, and leaves every other fabric usable. Removing the last fabric also
   clears Home Key trust and reopens commissioning.
5. Answer OperationalCredentials UpdateFabricLabel. The label is part of the
   fabric record, reads back through the Fabrics attribute, persists on its own
   without rewriting that fabric's network, ICAC or ACL records, and a label
   another fabric already holds is refused as LabelConflict in the NOCResponse.
6. Preserve the SRP client key and host identity together. Service structures
   remain allocated until OpenThread returns them in its removal callback, and
   duplicate registration is retried without reporting false success.

The DWM3001CDK Zephyr and FreeRTOS builds share
`modules/ultrawidelock_matter/`, `ports/zephyr/matter/matter_thread_port.c`, and
the numeric MF2 record layout in `ports/zephyr/store/matter_fab_settings.c`,
whose ids live in `ultrawidelock_kv.h`. The
ESP32 and nRF5340 ports delegate fabric transactions to their CHIP SDKs; their
application glue still owns the last-fabric credential cleanup rule.

The fabric record's length is itself the compatibility check: it grew when the
label field arrived, and the loader drops a record whose stored length is not
the one it expects rather than half-reading it. `FAB_VERSION` did not move.
Growing that struct therefore costs one re-pair, and a port that widens it owes
its users that warning.

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
