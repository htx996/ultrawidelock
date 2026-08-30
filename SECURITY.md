# Security policy

UltraWideLock is lock firmware. A defect here can be the difference between a
door that opens for its owner and one that opens for anyone, so please report
suspected vulnerabilities privately rather than in a public issue.

## Reporting a vulnerability

Use GitHub's private vulnerability reporting on this repository:
**Security → Report a vulnerability**. That opens a private advisory visible
only to you and the maintainers. No account beyond your GitHub login is needed.

Please do not open a public issue, pull request, or discussion for a suspected
vulnerability until an advisory for it has been published.

Useful things to include, in rough order of value:

- The commit or tag you tested, and the application and board.
- Which seam is involved: credential protocol, UWB ranging, BLE transport,
  Matter, DFU, or credential storage.
- A reproduction. A host-suite test case under `tests/host/` is the fastest
  possible report; a serial log or a described sequence is fine too.
- Whether the attack needs physical proximity, a paired credential, or neither.

## Supported versions

The SDK is pre-1.0. Only the latest release and `main` are supported; fixes are
not backported to earlier `0.x` series. See `VERSION` for the current version.

## Scope

In scope, as project-original code:

- The credential protocol implementation and its TLV codec in `modules/`.
- UWB ranging and the distance-bounding path, including replay and relay
  handling.
- Platform backends in `ports/`, and the five HAL seams named in `PORTING.md`.
- Signed update and rollback behavior, and anything that weakens the boot chain.
- Credential storage, key lifetime, and key material reaching a log or a
  console.

Out of scope here, because the code is not ours to fix. Please report these
upstream, though we are glad to know about them:

- The vendored Qorvo UWB driver under
  `modules/ultrawidelock_dw3000/dwt_uwb_driver/`: report to Qorvo.
- `modules/ultrawidelock_dfu/src/detools/` and its bundled heatshrink.
- nRF Connect SDK, Zephyr, ESP-IDF, esp-matter, OpenThread, Mbed TLS, and
  NimBLE: report to their respective projects.
- Findings that require a debugger on an unprotected part. Production images
  are expected to set access-port protection; `scripts/check-approtect.sh`
  exists for exactly that check.

## Known accepted risks

Weighed and kept deliberately. They are recorded here so that finding one is not
mistaken for finding a vulnerability. Reasoning lives beside the code.

- **The DFU receiver takes an ownership claim before anything is authenticated.**
  Whoever sends BEGIN first holds the receiver until they disconnect or the
  update window shuts, because the header signature is only checked once the
  full header has arrived. It is denial of an update, never a forged one: the
  ECDSA-P256 check and the magic/ABI/CRC checks both still stand. It is bounded
  by an owner-gated window and by `CONFIG_BT_MAX_CONN=1`, which means a peer that
  can send BEGIN has already taken the board's only connection slot. The obvious
  fix is barred: requiring an encrypted link would mean pairing, and the reader
  deliberately never asks a phone to pair. **Revisit if `CONFIG_BT_MAX_CONN` ever
  exceeds 1.** See `begin_at()` in `modules/ultrawidelock_dfu/src/dfu_receiver.c`.
- **`CONFIG_ULTRAWIDELOCK_CRED_DEV_TRUST` is a build-time authentication bypass.**
  It lets the built-in development identity authenticate against an empty trust
  store, for bring-up on a board with nothing provisioned. It is **LAB ONLY** and
  must be off in anything you ship. One arm of `tests/shared/run.sh` enables it
  on purpose.
- **The STS-quality floor has never been sized from real captures.** The
  DWM3001CDK now enforces the range-integrity gate rather than shadowing it, but
  the threshold it enforces is still the permissive default, so layer 2 rejects
  only what the DW3000 driver itself calls bad. See
  [`docs/range-integrity.md`](docs/range-integrity.md).

## Hardening expectations

Anyone shipping this firmware is responsible for provisioning their own MCUboot
signing key, enabling access-port protection, and not shipping the demo
commissioning values. The repository's defaults are bench defaults.
