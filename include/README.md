# Installed C API

`include/ultrawidelock/ultrawidelock.h` is the package umbrella. The role
declarations stay with their implementation owners:

| Include | Canonical source |
|---|---|
| `<ultrawidelock/reader.h>` | `modules/ultrawidelock_cred/include/ultrawidelock/reader.h` |
| `<ultrawidelock/device.h>` | `modules/ultrawidelock_cred/include/ultrawidelock/device.h` |
| `<ultrawidelock/tlv.h>` | `modules/ultrawidelock_cred/include/ultrawidelock/tlv.h` |
| `<ultrawidelock/uwb.h>` | `modules/ultrawidelock_uwb/include/ultrawidelock/uwb.h` |
| `<ultrawidelock/ultrawidelock_hal.h>` | `modules/ultrawidelock_port/include/ultrawidelock/ultrawidelock_hal.h` |

Source builds and installed packages share one `<ultrawidelock/...>` spelling;
the SDK test rejects flat role headers.

The package installs these entry points plus only the lower-level headers they
include. `make sdk-check` ratchets that exact set: adding or removing a package
header requires an explicit package and test update.

Headers only. Implementation stays in `modules/*/src/`.
