# Make target implementations

The top-level `Makefile` dispatches to these fragments. Invoke targets from the
repository root; the fragments are not standalone Makefiles.

| File | Responsibility |
|---|---|
| `cdk.mk` | DWM3001CDK build, flash, monitor, DFU, and size targets |
| `freertos-nrf52833.mk` | the Zephyr-free nRF52833 port: source checks, build, sign, flash, DFU |
| `nrf5340dk.mk` | nRF5340 DK lock and initiator targets |
| `esp32.mk` | ESP32 lock, reader, satellite, and initiator targets |
| `anchor.mk` | two-anchor bench targets |
| `satellite.mk` | the satellite responder: build, size, flash, console |
| `witness.mk` | the BLE witness dongle: build, flash, provisioning help |
| `web.mk` | the site, the WASM twin, and the subsystem graph |
| `host.mk` | host tests, coverage, CBMC, and architecture gates |
| `setup.mk` | tool inspection, workspace bootstrap, and signing-key setup |
| `extras.mk` | cleanup and remaining cross-cutting utilities |

The regression tiers cross those files. Each is a superset of the one above it;
`tests/README.md` says what each covers.

| Target | In | Needs |
|---|---|---|
| `make ci` | `host.mk` | a C compiler and python3, which is what a pull request is judged by |
| `make regress` | `host.mk` | the NCS toolchain, `./workspace`, a signing key |
| `make fw-regress` | `extras.mk` | the same, and nothing else: every DWM3001CDK image plus the size gate |
| `make regress-hil` | `nrf5340dk.mk` | a reader on its probe and the DK as the phone |
