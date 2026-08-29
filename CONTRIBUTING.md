# Contributing

What a change needs to be mergeable. The architecture contract is in
[`AGENTS.md`](AGENTS.md); the board and chipset workflow is in
[`PORTING.md`](PORTING.md).

## What you need

The host suites need a C compiler and `python3`:

```sh
make check
```

Run `make tools` to see every host tool, which targets each one gates, and what
is already installed on your machine. `llvm-cov` and `cbmc` gate `make coverage`
and `make cbmc`. `cppcheck` is optional but worth installing: `lint` skips loudly without it, and
CI runs it anyway. Zephyr builds need nRF Util, which installs the NCS toolchain
and which `make bootstrap` offers to install; ESP32 builds need ESP-IDF and, for
the Matter lock, esp-matter.

A change confined to `modules/` or `tests/` is fully verifiable with the host
suites alone.

## Before you open a pull request

Run the narrow check first, then verify in proportion to risk:

```sh
make sdk-check
bash tests/tooling/port_purity_check.sh --self-test
make check
```

After touching a port or an application, build the target it affects:
`make build`, `make nrf-build`, or `make esp-build APP=... TARGET=...`. ESP port
integration has `bash tests/ports/esp32/verify_port.sh`; the Zephyr port checks
under `tests/ports/zephyr/` are already part of `make check`. Say in the pull
request which of these you ran, and on what hardware if any.

## Architecture rules

The five numbered rules are in [`AGENTS.md`](AGENTS.md#architecture-contract),
and the purity gates enforce them.

> [!WARNING]
> Do not edit `modules/ultrawidelock_dw3000/dwt_uwb_driver/` or
> `modules/ultrawidelock_dfu/src/detools/`. Those are vendored.

## Change discipline

- Keep the diff to what the change needs. No drive-by reformatting.
- Do not weaken a test, a purity rule, or a ratchet allowlist to get a pass. A
  stale allowlist entry is a failure, and the change that made it unnecessary
  should remove it.
- Never commit private information, credentials, machine-local paths, or personal
  identity into files, output, or commit messages. Captures (`.pcap`, `.frc`,
  bench logs) are gitignored for this reason; a `.frc` carries live key material.
- Changing `VERSION` requires proving both source-tree and installed consumption
  with `make sdk-check`.

## Reporting bugs

Open an issue with the template. For anything with a security consequence, use
the private reporting path in [`SECURITY.md`](SECURITY.md) instead.

## License

By contributing you agree that your contribution is licensed under the ISC
license in [`LICENSE`](LICENSE), and that you have the right to license it that
way.
