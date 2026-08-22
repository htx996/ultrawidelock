# nRF52833 FreeRTOS Port

This directory owns the standalone FreeRTOS backends for the DWM3001CDK.
Shared protocol code selects them with `ULTRAWIDELOCK_PORT_FREERTOS`; it must not include
Nordic, Qorvo, or FreeRTOS headers directly.

The implemented foundation now includes:

- `include/ultrawidelock_freertos_platform.h` defines the BSP-owned clock, cycle-counter,
  busy-wait, flash, and logging hooks. Flash is BSP-owned because MPSL has to
  arbitrate the NVMC stall against radio timeslots.
- `osal/osal_freertos.c` implements deferred work, delayable work, semaphores,
  init hooks, and static-stack threads using only FreeRTOS APIs.
- `thread/openthread_freertos.c` owns one upstream OpenThread instance on a
  static task, serializes public API access with a recursive mutex, and provides
  task and ISR wake paths for tasklets, alarms, and radio work.
- `ble/nimble_sdc_transport.c` implements the static FreeRTOS receive task,
  controller serialization, NimBLE buffer ownership, ACL flattening, and MPSL
  task/ISR wake contract for the SoftDevice Controller HCI boundary.
- `radio/mpsl_freertos.c` owns the highest-priority static MPSL worker and the
  recursive lock shared by BLE, 802.15.4, and MPSL low-priority processing.
- `radio/radio_start_freertos.c` sequences the shared radio: it claims the
  frozen interrupt priorities, initializes MPSL from the board crystal, starts
  the worker on `mpsl_low_priority_process`, links only the peripheral
  GATT/CoC controller features, configures one peripheral link with 251-byte
  Link Layer packets, registers the hardware entropy source, enables the
  controller from a static eight-byte-aligned pool, and publishes the HCI
  transport contract. Every stage failure returns its own negative
  `ultrawidelock_freertos_radio_stage` instead of continuing.
- `ble/nimble_host_freertos.c` starts the whole BLE stack:
  `ultrawidelock_freertos_nimble_host_start()` brings up the radio, runs
  `nimble_port_init()`, registers the host's sync and reset callbacks, creates
  the host task on a static stack, and schedules host/controller
  synchronization. It returns before the link is usable; wait for
  `ultrawidelock_freertos_nimble_host_synced()` before advertising.
- `ble/nimble_syscfg/syscfg/syscfg.h` is this product's NimBLE configuration.
  It states only the settings that must differ from upstream and reaches the
  rest with `#include_next`, so the vendor tree is neither copied nor patched;
  put this directory ahead of upstream's on the include path. Every role it
  sets has to agree with the controller features `radio_start_freertos.c`
  links, and `make freertos-ble-source-check` fails if an upstream default ever
  stops contradicting an override.
- `ble/hci_compat/` lets the pinned `hci_internal.c` opcode dispatcher compile
  byte for byte out of the vendor tree. It supplies the Bluetooth Core packet
  layouts, status codes, and OpCode Group Field split the dispatcher expects
  under Zephyr names, plus `ultrawidelock_freertos_hci_config.h`, whose Kconfig symbols
  must mirror the controller features the sequencer links. Nothing Zephyr is
  compiled or linked, and the vendor file is never patched, so re-pinning it is
  a plain re-fetch. `ble/hci_dispatcher_freertos.c` adapts it to the port's
  radio contract.
- `radio/nrf_802154_clock_freertos.c` implements the 802.15.4 clock platform on
  top of MPSL, which owns the CLOCK peripheral. It uses the source-selecting
  MPSL API because the pinned MPSL deprecates the older one. The low-frequency
  clock is already running by the time anything calls it, since `mpsl_init()`
  waits for the crystal, and stopping it is refused because it carries the
  FreeRTOS tick. With the MPSL-arbitrated service-layer binary only `init` and
  `deinit` are actually reached; the rest is the header's contract and is what
  the open-source scheduler variant would call.
- `radio/nrf_802154_lptimer_freertos.c` implements the service layer's
  low-power timer on RTC2. The service layer works in 64-bit ticks while RTC2
  counts 24 bits, so the counter is extended with an overflow count; reads mask
  the timer interrupt briefly, because the counter and the count are written by
  two contexts and no ordering of those writes is safe to read lock-free.
  Compare channel 0 carries the scheduled timer, channel 1 the timestamper
  synchronization event, and channel 2 the hardware task. A deadline already
  past, or too near for the compare write to be seen, pends the interrupt
  instead of waiting out a 512 second wrap. The hardware task is armed with its
  event enabled and its interrupt off, because PPI carries it to another
  peripheral with no CPU involvement; the channel it publishes to is allocated
  by the 802.15.4 driver core and passed in, so this port never picks one and
  cannot collide with MPSL. Unlike most nRF peripherals the RTC raises no event
  at all until EVTEN says so, so every channel is armed with both bits.
- The high-precision timer platform on TIMER1 is Nordic's own
  `nrf_802154_hp_timer.c`, pinned in `platform.lock.yml` and compiled from the
  vendor tree unmodified. Unlike the low-power timer it depends on nothing but
  nrfx and the Nordic HAL, so it needs no compatibility layer and is used rather
  than reimplemented.
- `thread/ot_compat/` and `thread/ot_kernel_freertos.c` carry Nordic's own
  OpenThread radio platform, `radio_nrf5.c`, which is the whole otPlatRadio
  implementation. It is pinned and compiled unmodified rather than rewritten:
  its Zephyr surface is one semaphore, one intrusive queue, an atomic bit
  array, four byte-order helpers, the logging macros and the assertions, and
  every one of those is implemented here over FreeRTOS. The semaphore picks its
  FreeRTOS entry point by reading IPSR, because the 802.15.4 driver callouts
  signal it from an interrupt; the queue and the atomics mask interrupts for
  the same reason. `ultrawidelock_freertos_ot_config.h` states the Kconfig selection this
  product resolves to, and `make freertos-ncs-source-check` fails if the
  vendor file ever reaches past what the shim covers.
- `thread/ot_radio_freertos.c` makes the three joins Zephyr would otherwise
  make: `ultrawidelock_freertos_openthread_radio_start()` brings the platform up, which
  must follow `ultrawidelock_freertos_radio_start()` because the 802.15.4 driver
  arbitrates the radio through MPSL and must precede
  `ultrawidelock_freertos_openthread_start()` because the task drains the platform on
  every pass; `ultrawidelock_freertos_openthread_process_drivers()` is that drain, and
  refuses to reach the platform before it is up; and `otSysEventSignalPending()`
  becomes a FreeRTOS notification, through the interrupt path when IPSR says a
  driver callout is signalling.
- `thread/ot_alarm_freertos.c` is OpenThread's millisecond alarm, carried by the
  port's delayable work rather than a second timer service: the callback only
  records the expiry and wakes the OpenThread task, and the stack's own
  callback runs from that task under the API lock. A deadline already past is
  recorded rather than scheduled, and the flag is cleared before the callback so
  a re-arm from inside it is not swallowed. The microsecond alarm is absent on
  purpose: it exists only under
  `OPENTHREAD_CONFIG_PLATFORM_USEC_TIMER_ENABLE`, which this product leaves at
  upstream's default of zero, and `make freertos-radio-source-check` fails if
  that default ever changes.
- `thread/ot_misc_freertos.c` is entropy, reset, and assertions. The reset
  reason is latched at the first call and RESETREAS cleared, because that
  register accumulates bits across resets and a reader that leaves it alone
  keeps reporting the first reason the part ever had; where several bits are
  set, the most specific one is reported. Resetting into the bootloader is
  refused rather than turned into an ordinary reboot, because the DFU backend
  has not defined a boot-mode signal yet.
- `storage/kv_flash_freertos.c` is the persistent key-value store the reader's
  provisioning blob and OpenThread's settings both need. It is an append-only
  log over two 8 KB logical pages, each spanning two physical erase pages, so a
  value is replaced by appending a newer record rather than by rewriting an
  older one; the newest record for a key wins, and
  compaction copies the live set to the other page and erases the first. Every
  order in it is chosen against a power loss: a record's state word is written
  after its payload, so a torn record is never mistaken for a complete one, and
  a compaction target's page header is written after its records, so an
  interrupted compaction loses the work and not the data. The store occupies
  the same 16 KB region the Zephyr oracle reserves at `0x7c000`, but its record
  format is its own and is not Zephyr NVS: a board reflashed from the Zephyr
  build finds no valid page, reformats, and loses whatever the Zephyr settings
  partition held.
- `storage/ultrawidelock_prov_kv.c` is the reader's provisioning backend, the standalone
  twin of the Zephyr port's `ultrawidelock_prov_settings.c`. The serialisation, the dev
  fallback and the trust logic are the portable `ultrawidelock_prov.c`; this file only
  moves that one blob under `ULTRAWIDELOCK_KV_KEY_CRED_PROV`. Every failure still leaves
  a usable dev identity, because a reader that will not boot is worse than one
  that boots unprovisioned, and a stored record longer than this firmware can
  write is refused rather than parsed. A factory reset deletes the reader key
  and the Matter records it owns without reformatting unrelated OpenThread
  settings. The SRP client key and generated service-name identity are retained
  and erased as one unit, so a new key is not paired with an old claimed name.
  A static assert holds `ULTRAWIDELOCK_PROV_BLOB_MAX` (700 bytes at
  `ULTRAWIDELOCK_TRUST_MAX` 6) inside one record, so raising the anchor limit fails the
  build rather than the first provisioning write.
- `radio/nrf_802154_irq_freertos.c` maps the driver's IRQ abstraction to CMSIS
  NVIC operations; `nrf_802154_misc_freertos.c` supplies entropy-seeded random
  and die-temperature callouts.
- `board/time_freertos.c` supplies the three time hooks, from two sources on
  purpose. `ultrawidelock_freertos_uptime_us` must never step backwards over the life of
  the product, so it is the FreeRTOS tick count extended past its 32-bit wrap,
  which gives it no horizon at all. `ultrawidelock_freertos_cycle_get_32` and
  `ultrawidelock_freertos_busy_wait_us` need resolution finer than a tick, so they read
  RTC1's counter directly; that is safe because an RTC counter free-runs once
  started and nothing here writes a register. The counter is 24 bits and the
  cycle hook reports 32, so a wrap between two nearby reads is carried in
  software, which is what a latency probe straddling the wrap would otherwise
  get wrong by 16 million counts. The Zephyr oracle's `k_cycle_get_32()` on this
  part is the same 32768 Hz RTC, so the DW3110 response-arm probe measures
  against the same 30.5 us resolution here as it does there. A busy wait rounds
  the request up to whole ticks and then adds one more, because the counter may
  be a hair from advancing when the spin starts and the DW3000 driver asks for
  waits as short as 20 us; coming back early violates a part's timing and fails
  intermittently on a bench. The RTC1 prescaler is stated as
  `ULTRAWIDELOCK_FREERTOS_BOARD_RTC_HZ` rather than probed, because the pinned Nordic HAL
  is not guaranteed to expose a prescaler read. A counter that never advances is
  fatal on first use rather than an unbounded spin: a lock that hangs during
  driver bring-up looks like dead hardware.
- `board/entropy_freertos.c` is the hardware entropy source on RNG, and it keeps
  a pool rather than polling bare. The SoftDevice Controller registers it as its
  randomness source and the 802.15.4 driver seeds itself from it, and neither
  states which context it will ask from; the RNG with bias correction takes on
  the order of a hundred microseconds per byte, so a caller blocking for that at
  a radio interrupt priority would overrun a radio event. Filling from the RNG's
  own vector makes the ordinary request a copy. Bias correction is on, which
  costs throughput and buys uniformly distributed bits: the reader's key
  material comes from here. A caller that outruns the pool polls the peripheral
  directly with only the RNG vector masked, so the handler cannot take the byte
  it is waiting for while the radio keeps running. The generator stops once the
  pool is full and starts again when it drains, and a byte already in flight as
  it stops is dropped rather than written over the oldest entry.
- `board/temperature_freertos.c` reads the die temperature from MPSL, not from
  TEMP. MPSL owns that peripheral and takes its own readings to calibrate the
  low-frequency clock, and TEMP answers one measurement at a time, so a second
  reader driving `TASKS_START` underneath it would corrupt the timebase the
  whole radio rests on. MPSL reports quarter degrees; the 802.15.4 driver, the
  only consumer, wants whole ones.
- `board/flash_freertos.c` is internal flash on NVMC, arbitrated against the
  radio with MPSL timeslots. Programming this flash stalls the CPU: a word write
  is tens of microseconds and a page erase is 89.7 ms, which is longer than MPSL
  will ever grant in one piece, so an operation issued while the radio is
  scheduled would overrun a radio event. With the radio down there is nothing to
  arbitrate and the work runs directly; with it up it is cut into pieces and
  each piece runs inside a timeslot. A write fills as much of its slot as it
  measures room for, using the TIMER0 that MPSL resets at the start of every
  slot, which beats assuming a programming time this port cannot verify. An
  erase uses the controller's partial-erase mode: 89.7 ms of erasing in thirty
  3 ms slices with the CPU free between them, rather than one stall that would
  drop a BLE connection. A blocked or cancelled request is retried rather than
  failed, because abandoning the work would leave a partially erased page that
  reads as neither the old contents nor the new. Two things differ from Nordic's
  own Zephyr driver deliberately: that driver gives a kernel semaphore from
  inside the timeslot callback, which this port does not, because the callback
  runs at interrupt priority zero and FreeRTOS forbids its API above
  `configMAX_SYSCALL_INTERRUPT_PRIORITY`; the callback here touches only NVMC
  and a flag, and the calling task polls it. The controller's mode is set and
  restored around every operation, since one left in write mode turns any later
  stray store into a flash program. Writes and erases are confined to a window
  that excludes the application image, so a store with an offset bug can lose
  its own data but cannot erase the firmware out from under a door lock; a board
  that adds a DFU slot widens `ULTRAWIDELOCK_FREERTOS_FLASH_WRITABLE_BASE` and `_LIMIT`
  deliberately. Reads need neither the window nor the controller, since program
  flash is memory-mapped.
- `board/log_rtt_freertos.c` is the log sink, as a SEGGER RTT up-buffer. RTT and
  not the UART, and that is the board's decision rather than a preference:
  `uart0` is the J-Link OB's virtual COM port and MCUboot's serial recovery
  rides it, so an application console there would collide with the DFU path.
  The Zephyr oracle makes the same choice for the same reason. The control block
  is written here against the published RTT layout rather than by linking
  SEGGER's implementation, which is licensed for use with SEGGER's own products;
  nothing is vendored and no third-party source is compiled, and a J-Link finds
  this by scanning RAM for the identifier exactly as it finds SEGGER's. The
  buffer is in no-block-skip mode, which is the only safe choice: a blocking
  sink would stall its caller, and at 115200 baud an 80-character line is about
  7 ms against a 1.836 ms DW3110 response-arm deadline. A line that does not fit
  is dropped whole, because a partial line would splice with the next writer's
  and a log that invents lines is worse than one that admits it lost some. The
  write offset never reaches the read offset, since equal offsets are how a host
  is told the buffer is empty. With no debugger attached the host never advances
  the read offset, the buffer fills once, and every later write costs a bounds
  check. A write masks interrupts for the length of the copy, roughly a
  microsecond per hundred bytes at 64 MHz, so this hook must not be called from
  the radio's priority-zero handlers. The struct offsets are a wire format and
  are pinned by static assert on a 32-bit target.
- `board/fault_freertos.c` resets rather than halts, because this is a door
  lock: a board spinning in a fault has stopped answering, while one that
  reboots comes back and can be opened. Define `ULTRAWIDELOCK_FREERTOS_FATAL_HALT` for
  bench builds to stop instead. The reset shows up in RESETREAS as SOFTWARE,
  which `otPlatGetResetReason` then reports.
- `crypto/` selects and starts the crypto provider: Mbed TLS 3.6.6, built
  standalone from its own CMake with the PSA core on. The provider is chosen for
  what already compiles against it rather than for speed:
  `modules/ultrawidelock_cred/src/ultrawidelock_prim_psa.c` is the reader's primitive backend and
  speaks PSA, so a PSA core reuses it unchanged, exactly as `ports/esp32` does
  over ESP-IDF's Mbed TLS. OpenThread is deliberately left on its upstream
  default of `OPENTHREAD_CONFIG_CRYPTO_LIB_MBEDTLS` rather than moved to PSA,
  and that single choice is what keeps Zephyr's `crypto_psa.c` out of the build
  and removes any need for persistent PSA keys, `MBEDTLS_PSA_CRYPTO_STORAGE_C`,
  or an ITS backend: OpenThread's SRP client was the only caller that asked for
  `PSA_KEY_LIFETIME_PERSISTENT`, and all 83 PSA call sites in `ultrawidelock_prim_psa.c`
  import, use, and destroy volatile keys. OpenThread's key material lands in
  `otPlatSettings` over the key-value store instead, which means Thread
  credentials are stored differently here than in the Zephyr oracle and a board
  reflashed between the two loses them. There is no CryptoCell on this part, so
  P-256 is software and an asymmetric operation costs tens of milliseconds; that
  is affordable because the credential protocol's expedited path uses Kpersistent with AES-CMAC
  and does no asymmetric work inside the 1.836 ms DW3110 response-arm window.
  `nrf_oberon` would be faster and adds no licence this port does not already
  carry, but the glue that binds it under the PSA core is `nrf_security`, which
  is Zephyr Kconfig, so it stays out until something is measured to need it.
  `mbedtls_config_freertos.h` replaces the library defaults wholesale and names
  no legacy module, because `config_adjust_legacy_from_psa.h` derives them from
  the `PSA_WANT_*` list; AES tables go in flash because RAM is the binding
  constraint on this part. `mbedtls_threading_freertos.c` supplies the
  `MBEDTLS_THREADING_ALT` callbacks over static FreeRTOS mutexes, needed for
  correctness rather than hardening: the OpenThread task and the credential task
  behind NimBLE share the PSA key store. A lock from an exception is refused
  rather than allowed to assert the scheduler.
  `mbedtls_platform_freertos.c` puts allocation on the FreeRTOS heap NimBLE
  already forces, and makes the board entropy source the library's only seed.
  `make freertos-crypto-source-check` asserts the contracts the port cannot see
  from here: the `threading_alt.h` include spelling, the `mbedtls_hardware_poll`
  signature, and that every `PSA_WANT_*` the config sets is one Mbed TLS
  recognises. That last row earned itself immediately by catching
  `PSA_WANT_GENERATE_RANDOM`, which is Nordic's Kconfig and not an Mbed TLS
  option, copied in from the oracle's list where it would have been silent.
- `peripherals.yml` freezes RTC, TIMER, EGU, vector, and clock ownership before
  the first target link.
- `platform.lock.yml` pins the Qorvo base and the exact OpenThread, nrfxlib,
  SDC opcode-dispatcher, Nordic HAL, Mbed TLS, and NimBLE revisions.
- `make freertos-port-test` compiles and runs this production backend against a
  recording FreeRTOS test double.

Every kernel object this port owns is statically allocated, but the build is
not heap-free. Apache NimBLE's FreeRTOS porting layer creates its own objects
with the dynamic APIs: one 32-entry event queue, the host, HCI, and GAP
preemption mutexes, the HCI acknowledgement semaphore, and one software timer
per callout. Patching the vendor tree is not an option, so the board build must
set `configSUPPORT_DYNAMIC_ALLOCATION=1` and `configUSE_TIMERS=1` and provide a
heap. Those allocations all happen once inside `nimble_port_init()` and are
never freed, so the heap is a fixed startup cost, not a source of runtime
fragmentation. `make freertos-ble-source-check` asserts the allocation sites so
a NimBLE bump that adds more has to be re-costed. The exact heap size is a
first-link measurement and is not yet established.

The dispatch task defaults to a
4096-byte stack and priority `tskIDLE_PRIORITY + 2`; the eventual board build
may override `ULTRAWIDELOCK_FREERTOS_OSAL_STACK_BYTES`,
`ULTRAWIDELOCK_FREERTOS_OSAL_QUEUE_DEPTH`, and `ULTRAWIDELOCK_FREERTOS_OSAL_TASK_PRIORITY`. The
NimBLE host task defaults to a 4096-byte stack at `tskIDLE_PRIORITY + 1`,
below the HCI receive pump, and is overridden with
`ULTRAWIDELOCK_FREERTOS_NIMBLE_HOST_STACK_BYTES` and
`ULTRAWIDELOCK_FREERTOS_NIMBLE_HOST_TASK_PRIORITY`.

## Platform architecture

The 2026-08-10 scope decision accepts a maintained custom radio port. Qorvo
DW3/QM33 SDK v1.1.1 is the pinned DWM3001CDK board, DW3000, FreeRTOS, and nrfx
source base. This port supplies the OpenThread, 802.15.4 coexistence, and
512-byte L2CAP CoC integration. The shipping radio architecture uses linkable
MPSL/SoftDevice Controller, the nRF 802.15.4 service layer, and Apache NimBLE
1.10.0, which keeps standalone MCUboot at address zero. Bundled S113 is only an
interim bring-up option because its fixed application origin breaks that map.
The complete evidence and remaining gates are in
[`internal/dwm3001cdk-freertos-platform-qualification.md`](../../internal/dwm3001cdk-freertos-platform-qualification.md).

Reproduce the Qorvo artifact audit with:

```sh
make freertos-platform-check \
  QORVO_SDK_ARCHIVE=<path-to-DW3_QM33_SDK_1.1.1.zip>
```

Verify an existing NCS workspace against the radio source pins with:

```sh
make freertos-radio-source-check NCS_WORKSPACE=<path-to-ncs-workspace>
```

Verify an Apache NimBLE checkout against the BLE host pin with:

```sh
make freertos-ble-source-check NIMBLE_SOURCE=<path-to-mynewt-nimble>
```

The check verifies that the exact archive contains the board/UWB/FreeRTOS base,
S113 L2CAP CoC API, and radio Timeslot API. It explicitly reports OpenThread,
802.15.4 coexistence, and CoC target integration as port-owned work. Release
still requires all of the following on the DWM3001CDK:

1. BLE peripheral GATT plus a 512-byte L2CAP CoC.
2. OpenThread MTD attachment while BLE remains connected.
3. Safe flash operations and USB CDC under concurrent radio activity.
4. A high-resolution monotonic timer and cycle counter suitable for measuring
   the DW3110 response-arm path.
5. Reproducible licensing, source acquisition, and an ARM GCC build.

MPSL and the SoftDevice Controller are started by `ultrawidelock_freertos_radio_start()`,
which takes the opcode dispatcher pair. Pass
`ultrawidelock_freertos_radio_sdc_dispatcher()` for the pinned Nordic dispatcher. The
board layer's remaining radio duty is routing RADIO, RTC0, TIMER0, POWER_CLOCK,
and SWI5_EGU5 to the `ultrawidelock_freertos_radio_*_isr()` entry points. The transport
intentionally rejects ISO packets because the product requires BLE GATT and
L2CAP CoC, not LE Audio.

The BLE configuration is peripheral-only on purpose: no central, no observer,
no extended advertising, one connection, one credit-based L2CAP channel, and
LE Secure Connections with the legacy pairing fallback compiled out. Bonding
stays off until the port has a settings backend to persist keys into. Privacy
has no build-time switch, so product code must pass only
`BLE_OWN_ADDR_PUBLIC` or `BLE_OWN_ADDR_RANDOM`; the controller does not link
`sdc_support_le_privacy` and has no resolving list, so requesting an
`BLE_OWN_ADDR_RPA_*` type fails at runtime rather than at build time.

The transport copies each HCI event into one fixed `BLE_TRANSPORT_EVT_SIZE`
pool block. Upstream's 70-byte default is exactly the Command Complete for Read
Local Supported Commands, the largest event a legacy-only peripheral receives,
so it is correct here but has no margin. The transport refuses a longer event
instead of copying it, because that bound is a property of the linked feature
set rather than of the copy.

Reception must go through the dispatcher's `msg_get` and never through
`sdc_hci_get` directly. The controller's command API is opcode-specific, so the
dispatcher answers most commands itself and stages the resulting Command
Complete or Command Status in its own buffer; a read path that skipped it would
lose every command response.

Build and run the pinned NCS sources against an NCS workspace with:

```sh
make freertos-ncs-source-check NCS_WORKSPACE=<path-to-ncs-workspace>
```

The controller pool is 4096 bytes. The pinned `SDC_MEM_*` macros put the
shipping configuration at 3078 bytes, and `ultrawidelock_freertos_radio_start()` fails
with `ULTRAWIDELOCK_FREERTOS_RADIO_STAGE_SDC_MEMORY` rather than overrunning the pool if
a controller update ever needs more. Override `ULTRAWIDELOCK_FREERTOS_SDC_MEM_BYTES` to
resize it.

The shipping resource map keeps Qorvo's RTC1 FreeRTOS tick, assigns RTC0 and
TIMER0 to MPSL, and assigns RTC2, TIMER1, and EGU0 to nRF 802.15.4. Qorvo's
`app_timer` must be omitted because it currently occupies RTC2; delayable
application work already uses the port OSAL. MPSL uses SWI5/EGU5 for its
low-priority signal, and that IRQ must be configured at a FreeRTOS-callable
priority before it invokes `ultrawidelock_freertos_mpsl_wake_from_isr()`.

## Target build

```sh
make freertos-build \
  NCS_WORKSPACE=<path-to-ncs-workspace> \
  QORVO_SDK_DIR=<path-to-extracted-DW3_QM33_SDK_1.1.1>
```

The image was assembled in layers, and a layer was only added once the one below
it linked. The binding constraint is 512 KB of flash and 128 KB of RAM. The
current Zephyr production oracle fits at 120,740 B of RAM with 10,332 B spare;
FreeRTOS must still be measured from its own ELF because its task and storage
layout differs. The base layers below are linked and have hardware evidence.

What is linked:

| Layer | Contents |
| --- | --- |
| kernel | FreeRTOS V10.0.0 and its Cortex-M4F port, compiled unmodified |
| device | the pinned hal_nordic nrfx, MDK, and CMSIS |
| board | startup and vector table, RTC1 tick, timebase, RTT log, faults, entropy, die temperature |
| radio | MPSL, the peripheral SoftDevice Controller, the FEM dispatch layer, and the pinned NCS opcode dispatcher |
| crypto | Mbed TLS 3.6.6, libmbedcrypto sources only, PSA core on |
| BLE | Apache NimBLE host, porting layer, and transport |
| storage | MPSL-arbitrated NVMC, the key-value store, and the provisioning record |
| UWB | the ranging engine, the vendored decadriver, and the DW3110 backends |
| Thread | OpenThread as an MTD, built through its own CMake, plus the port's adapters |

The recorded non-Matter image is 418,584 bytes of flash and 108,092 of RAM: 96 percent of
the 0x6a000 application slot and 82 percent of the RAM budget. Flash is the tight
one, with under 16 KB spare. `FREERTOS_MATTER=ON` selects the separate Matter
variant and links the portable commissioning stack; do not use the non-Matter
number as its budget. A credential is typed over USB only in the non-Matter
console build.

`ultrawidelock_uwb_reach` still runs on every build and still reports an upper bound
rather than a cost, because `--gc-sections` charges only what is reachable and a
responder that never initiates cannot reach all of the decadriver.

VERIFIED ON A DWM3001CDK, not inferred from a link: MCUboot hands over and the
image boots, the store completes a page erase in thirty MPSL timeslot slices
with none blocked, the DW3110 answers SPI with DEV_ID 0xdeca0302 and configures
its PHY, OpenThread's radio initialises, the reader advertises and serves its
GATT table over a real connection, the update channel refuses every opcode with
no window open, MCUboot's serial recovery answers an image-list request, the
status LEDs run their patterns, and the grant decision executes -- confirmed by
a breakpoint on `grant_step`, not by a log line. `scripts/freertos-ble-liveness.py`
re-runs the over-the-air half on demand, because this port has twice printed a
complete, healthy boot log from a board that had already stopped.

The RAM figure is the one that matters. The first link with the BLE host in it
also found 3,480 bytes of isochronous transport buffers that upstream allocates
by default and that nothing in this product can use, since the transport rejects
ISO packets outright; `ble/nimble_syscfg` now sets those counts to zero. The
next candidate is the ACL pool at 3,000 bytes, which is upstream's ten blocks
for a build with one connection, but that one is a throughput trade rather than
dead weight and should be measured before it is cut.

The cross toolchain is found on `PATH`. Set `ULTRAWIDELOCK_ARM_TOOLCHAIN_DIR` to a
toolchain's `bin` directory to use one installed outside the system prefix
without putting it on `PATH` for everything else.

Three vendor trees feed the build and none of them is copied into the
repository: the extracted Qorvo SDK, the west workspace, and the Apache NimBLE
checkout, each pinned by `platform.lock.yml`.

`board/nrf52833_lock.ld` stops the image at `0x74000`. The 16 KB region at
`0x7c000` is the key-value store, which holds the credential provisioning
record and OpenThread's settings including the SRP client key, so an oversized image is a
link error rather than a lock that forgets its identity after a firmware update.
MCUboot occupies the bottom of flash and the application runs from `0x0a000`
with its vector table relocated, which is why the store sits at the top and out
of the way of both.

The kernel is FreeRTOS V10.0.0 from the pinned Qorvo tree's nRF5 SDK, and its
Cortex-M4F port is compiled unmodified against two small headers in
`kernel/kernel_compat/`, the same arrangement `ble/hci_compat` and
`thread/ot_compat` use for the pinned NCS sources. One vendor file is
deliberately left out: `portable/CMSIS/nrf52/port_cmsis_systick.c` drives RTC1
through `nrf_drv_clock`, which fights MPSL for the low-frequency clock, and it
prescales the counter `board/time_freertos.c` reads at full rate.
`board/tick_freertos.c` replaces it, running RTC1 unprescaled at 32768 Hz with
the tick on a compare stepped 32 counts at a time. That is why
`configTICK_RATE_HZ` is 1024 and not 1000: 32768 divides by 1024 exactly, and a
static assertion in the tick file refuses any rate that does not.

`configMAX_SYSCALL_INTERRUPT_PRIORITY` is 4, and it is pinned from both sides.
It cannot exceed 4 because the MPSL low-priority handler runs at 4 and calls
`vTaskNotifyGiveFromISR`; it cannot go below 2 because a critical section at
that ceiling would mask MPSL at 0 or the nRF 802.15.4 driver at 1. Both bounds
are static assertions, one in `board/FreeRTOSConfig.h` and one in
`radio/radio_start_freertos.c`, placed where the numbers they compare are
visible. Priorities 0 through 3 are radio-only by construction.

The deprecated Nordic nRF5 Thread SDK is reference material only; its bundled
OpenThread build is not a shipping dependency. The pinned current OpenThread,
MPSL, SoftDevice Controller, and nRF 802.15.4 components must be integrated
through their RTOS-independent APIs without importing Zephyr into the release.
