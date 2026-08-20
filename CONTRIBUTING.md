# Contributing

Thanks for looking at UltraWideLock. This file is the short version of what a
change needs to be mergeable. The architecture rules it references live in
[`AGENTS.md`](AGENTS.md), and the board and chipset workflow lives in
[`PORTING.md`](PORTING.md).

## What you need

Nothing beyond a C compiler and `python3` is required to run the host suites:

```sh
make check
```

Run `make tools` to see every host tool, which targets each one gates, and what
is already installed on your machine. `llvm-cov` and `cbmc` gate `make coverage`
and `make cbmc`. `cppcheck` is worth installing even though it is optional: the
`lint` suite inside `make check` skips loudly without it and CI runs it anyway,
so a missing local copy means finding out on the pull request instead. Zephyr
builds additionally need nRF Util, which installs the NCS toolchain; ESP32
builds need an installed ESP-IDF and, for the Matter lock, esp-matter.

Target builds are not required to contribute. A change confined to `modules/`
or `tests/` is fully verifiable with the host suites alone.

## Before you open a pull request

Run the narrow check first, then verify in proportion to risk:

```sh
make sdk-check
bash tests/tooling/port_purity_check.sh --self-test
make check
```

If you touched a port or an application, also build the target it affects:
`make build`, `make nrf-build`, or `make esp-build APP=... TARGET=...`. ESP port
integration has `bash tests/ports/esp32/verify_port.sh`; the Zephyr port checks
under `tests/ports/zephyr/` are already part of `make check`. Say in the pull request
which of these you actually ran, and on what hardware if any.

## Architecture rules

These are enforced by the purity gates, not just by review:

1. `modules/` names no operating system. Public headers in `include/`, private
   headers and implementation in `src/`.
2. `ports/zephyr/` names only Zephyr; `ports/esp32/` names only ESP-IDF.
3. A source must not include another module's `src/`, and build files must not
   propagate a module `src/` include directory.
4. Shared sources go in exactly one role manifest under `modules/*/roles/`. Do
   not duplicate source lists in a consuming build file.
5. Do not create a chipset-named top-level port when the chipset still uses a
   supported framework. Extend the framework port, keep board policy in the
   application.

Do not edit `modules/ultrawidelock_dw3000/dwt_uwb_driver/` or
`modules/ultrawidelock_dfu/src/detools/`. Those are vendored.

## Change discipline

- Keep the diff to what the change needs. No drive-by reformatting.
- Do not weaken a test, a purity rule, or a ratchet allowlist to get a pass. A
  stale allowlist entry is a failure, and it should be removed by the change
  that made it unnecessary.
- Never commit private information, credentials, machine-local paths, or
  personal identity into files, output, or commit messages. Captures (`.pcap`,
  `.frc`, bench logs) are gitignored for this reason; a `.frc` in particular
  carries live key material.
- Changing `VERSION` requires proving both source-tree and installed
  consumption with `make sdk-check`.

## Reporting bugs

Open an issue with the template. For anything with a security consequence, use
the private reporting path in [`SECURITY.md`](SECURITY.md) instead.

## License

By contributing you agree that your contribution is licensed under the ISC
license in [`LICENSE`](LICENSE), and that you have the right to license it that
way.
