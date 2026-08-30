# Portable modules

The shared implementation; backends live in `ports/`. The purity gate keeps it
free of OS dependencies, ratcheting a small explicit set of legacy adapters.
`ultrawidelock_` prefixes module directories, `ULTRAWIDELOCK_*` Kconfig symbols
and Zephyr module names, so nothing collides with Zephyr, NCS or ESP-IDF.

| Module | Responsibility |
|---|---|
| `ultrawidelock_port` | OS, flash, logging, allocation, and byte-order contracts |
| `ultrawidelock_cred` | credential APDU, crypto, provisioning, reader, and approach logic |
| `ultrawidelock_cred_stack` | links the portable credential stack into the Nordic application |
| `ultrawidelock_uwb` | credential and FiRa UWB messages, sessions, ranging, and diagnostics |
| `ultrawidelock_dw3000` | DW3000-family driver source sets and portable driver seams |
| `ultrawidelock_nfc` | NFC transport abstraction, PN532 transport, and the RFAL-path ECP emitter |
| `ultrawidelock_matter` | minimal Matter transport, session, and cluster implementation |
| `ultrawidelock_anchor` | side of door: geometry, fusion, latch, seal, witnesses, SLAM |
| `ultrawidelock_ml` | LOS/NLOS feature extraction and classifier |
| `ultrawidelock_dfu` | delta update receiver and applier |

## Boundaries

- `include/` is the module's public API.
- `src/` is private, and may not be included by another module.
- `roles/*.list` is the authoritative source membership for selectable roles.
- `zephyr/` is build-system discovery metadata, not a backend.

`ultrawidelock_port/include/` is headers-only; every implementation of those
contracts belongs in a port tree or the host test backend. Its
`include/ultrawidelock/ultrawidelock_hal.h` names the five chipset seams. Each
module owns its canonical SDK headers under its own `include/ultrawidelock/`.
