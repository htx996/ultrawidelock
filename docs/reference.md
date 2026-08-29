# Reference

The exact shape of something is in the header that declares it. This repository
has no generated API tree: the declarations carry per-field comments, and a
second rendering of them is one more thing that can fall behind the code.

## Start at the umbrella

```c
#include <ultrawidelock/ultrawidelock.h>   // every public declaration
```

Or include a single role:

| Header | For |
|---|---|
| [`<ultrawidelock/reader.h>`](../modules/ultrawidelock_cred/include/ultrawidelock/reader.h) | the lock side: sessions, the key ladder, the unlock decision |
| [`<ultrawidelock/device.h>`](../modules/ultrawidelock_cred/include/ultrawidelock/device.h) | the initiator side, which is what a phone stand-in builds |
| [`<ultrawidelock/tlv.h>`](../modules/ultrawidelock_cred/include/ultrawidelock/tlv.h) | the codec alone, with no radio and no storage |
| [`<ultrawidelock/ultrawidelock_hal.h>`](../modules/ultrawidelock_port/include/ultrawidelock/ultrawidelock_hal.h) | the five seams a port implements |

`include/ultrawidelock/` owns the public surface. Anything reachable only from
`modules/*/include/` is internal by construction, and a port that reaches for
it fails `make purity`.

## Where the protocol lives

Most of the protocol is preprocessor constants rather than functions: attribute
IDs, frame lengths, key-block offsets, bitmask layouts. Read them where they are
defined.

| Looking for | Look in |
|---|---|
| credential APDUs, provisioning, the approach state machine | `modules/ultrawidelock_cred/include/` |
| CCC sessions, the key derivation ladder, DS-TWR, FiRa | `modules/ultrawidelock_uwb/include/` |
| side of the door: fusion, latch, seal, witnesses | `modules/ultrawidelock_anchor/include/` |
| the Matter node's clusters and transport | `modules/ultrawidelock_matter/` |
| ECP and the reader transports | `modules/ultrawidelock_nfc/` |
| the OS, flash, log and byte-order contracts | `modules/ultrawidelock_port/include/` |

Ports add their own surfaces under `ports/<port>/`: the ESP32 tree carries
`ultrawidelock_ble`, `ultrawidelock_crypto`, `ultrawidelock_reader`,
`ultrawidelock_satlink` and the target-specific board-pin API as ESP-IDF
components.

## The CMake package

```sh
cmake -S . -B build/sdk -DCMAKE_INSTALL_PREFIX="$PWD/build/sdk-install"
cmake --build build/sdk
cmake --install build/sdk
```

Exports `UltraWideLock::headers` and `UltraWideLock::tlv`; the version comes from
the root `VERSION`. `add_subdirectory` also works, and `make sdk-check` verifies
both paths against [`examples/cmake/consumer/`](../examples/cmake/consumer/).
Full firmware is consumed through the Zephyr module or the ESP-IDF components.

## Where to look

| Question | Where to look |
|---|---|
| What are the fields of this struct? What is this constant's value? | the header, above |
| Which files make up this subsystem, and what depends on what? | the [subsystem graph](https://ultrawidelock.com/graph/index.html), built by `make docs` |
| What can I include from outside the tree? | [`PORTING.md`](../PORTING.md) and `make sdk-check` |
| Why is it built this way? What went wrong on the bench? | the guides: [`esp32-gotchas.md`](esp32-gotchas.md) for the ESP32-S3 port, [`dwm3001cdk-surgery.md`](dwm3001cdk-surgery.md) for the DWM3001CDK |
| What is the wire format? | [`protocol-notes.md`](protocol-notes.md), then [`protocol-research.md`](protocol-research.md) |

If a declaration's shape is not obvious from its header, the fix is a doc
comment at the declaration.
