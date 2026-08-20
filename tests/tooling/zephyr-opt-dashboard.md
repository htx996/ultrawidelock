# DWM3001CDK optimization dashboard

`zephyr_opt_dashboard.py` turns the repository's existing DWM3001CDK size and
runtime evidence into a machine-readable run tree, a compact Markdown status,
and one self-contained HTML dashboard. The generated files default to
`internal/zephyr-opt/dashboard/` and remain local. The collector does not flash,
erase, monitor, install tools, refresh baselines, or write outside its output
directory.

## Evidence states

Every source is one of `measured`, `unavailable`, `failed`, or `not-run`.
Missing builds, hardware, and optional tools never become zero. Checked-in
baseline bytes are shown as reference-only when no current size report exists.
Size deltas are withheld when the board, image, overlays, Zephyr, NCS,
toolchain, or recorded Kconfig axes differ.

Synthetic data under `tests/tooling/fixtures/zephyr-opt/` exists only for tests
and visual verification. It must not be cited as a product measurement.

## One command on real hardware

From an interactive terminal with the DWM3001CDK connected through its J-Link
USB port, run:

```sh
make instrument
```

The target performs the complete bench lifecycle:

1. Generate the ignored P-256 signing key without overwriting an existing key,
   and bootstrap Zephyr only when `workspace/` is absent. An incomplete existing
   workspace is refused so this command cannot discard workspace edits.
2. Build a pristine `thread+lto` bench image with latency, UWB histogram, and
   DW3000 SPI instrumentation, then measure its linker flash and RAM.
3. Flash without erasing commissioned state and start RTT with the exact ELF.
4. Stream the raw capture under ignored `internal/zephyr-opt/captures/` until
   Enter is pressed, then ask for the honest physical-attempt denominator.
5. Collect Zephyr reports, optional `pahole` output, render the dashboard, bind
   it to `127.0.0.1`, and open the default browser. Press Enter again to stop
   the server and exit successfully.

The command refuses a non-interactive terminal before building or flashing.
If several probes are attached, set `PROBE_RS_PROBE=<VID:PID:Serial>` first.
`probe-rs` must already be on `PATH`. On a fresh checkout the bootstrap step can
download the pinned NCS workspace and toolchain, so the first run is much longer
than later runs. Useful overrides are:

```sh
make instrument \
  INSTRUMENT_ATTEMPTS=1000 \
  INSTRUMENT_WARMUP=20 \
  INSTRUMENT_REJECTED=0 \
  INSTRUMENT_TIMED_OUT=0 \
  INSTRUMENT_PORT=8765
```

Leaving `INSTRUMENT_ATTEMPTS` empty prompts after capture. An empty response
keeps the denominator explicitly unknown rather than inferring that every
physical attempt produced a log record. The command does not fabricate runtime
stack or CPU evidence; those still require the separate debugger snapshot
described below.

## Collect static evidence

Build the profile in its own pristine directory, then use the existing size
accounting. A release and a debug image are different experiments.

```sh
make build PRISTINE=1 CDK_BUILD=build/cdk-opt-debug
make cdk-size CDK_BUILD=build/cdk-opt-debug

python3 tests/tooling/zephyr_opt_dashboard.py collect \
  --build build/cdk-opt-debug \
  --run-zephyr-tools --run-pahole
```

`--run-zephyr-tools` inventories and invokes `ram_report`, `rom_report`, and
`dashboard` on the nested `dwm3001cdk-lock` image. It sets `BROWSER=true`,
checks that each artifact exists, and preserves sanitized JSON links where the
pinned Zephyr provides them. Without this flag, discovery is read-only and any
already existing artifacts are still indexed.

Use `--generate-size-report` when the ELF exists but `size-report.json` does
not. This runs `scripts/cdk-size.py` without changing the recorded baseline.
The canonical linker-region values remain separate from Zephyr's report totals
because the accounting methods are not identical.

The optional static-stack lane enables compiler `.su` output:

```sh
repo_root=$(git rev-parse --show-toplevel)
make build PRISTINE=1 CDK_BUILD=build/cdk-opt-static-stack \
  CDK_CONF="overlay-thread.conf;overlay-lto.conf;${repo_root}/tests/tooling/zephyr-opt-overlays/static-stack.conf"
```

Use Zephyr's `puncover` target as a temporary local explorer when it exists.
`--run-pahole` runs `pahole --sizes` against the nested ELF and attaches its
sanitized text report. These tools are optional diagnostics. Their absence does
not invalidate the region totals.

## Collect walk-up latency

The latency parser accepts only the compact `ultrawidelock-lat:` record emitted
by `modules/ultrawidelock_cred/src/ultrawidelock_lat.c`. Other log lines are
counted but never persisted. Supply the total attempt denominator and explicit
failure classes so dropped attempts cannot disappear.

```sh
repo_root=$(git rev-parse --show-toplevel)
make build PRISTINE=1 CDK_BUILD=build/cdk-opt-latency \
  CDK_CONF="overlay-thread.conf;overlay-lto.conf;${repo_root}/tests/tooling/zephyr-opt-overlays/latency-uwb-spi.conf"

python3 tests/tooling/zephyr_opt_dashboard.py collect \
  --build build/cdk-opt-latency \
  --latency-log internal/zephyr-opt/captures/control.log \
  --attempts 1000 --warmup 20 --rejected 0 --timed-out 0 \
  --profile thread+lto --experiment control
```

Do not use automatic thread analysis in this lane. Capture through the
repository's exact-ELF monitor flow only after hardware use has been authorized.
Retained UWB changes still need the repository dossier's 1,000-attempt walk-up
gate. The dashboard reports the supplied sample set; it does not weaken that
acceptance rule.

## Supply stack, CPU, UWB, and SPI evidence

Runtime measurements use the strict envelope documented by
`zephyr_opt_runtime_snapshot.schema.json`. Populate it from an authorized,
post-run debugger read of the exact flashed ELF. The collector never attaches
to a probe itself.

- Runtime stack watermarks come from stack paint or an explicitly named method.
- CPU percentages come from one stated observation window and method.
- `g_lat_prepoll`, `g_lat_resp`, and `g_lat_final` contain silent 250 us UWB
  histograms in DW3000 hi32 ticks, using 250 ticks per microsecond.
- `g_spi_poll_arm`, `g_spi_response_arm`, and `g_spi_final_arm` contain the
  optional per-leg SPI counters.
- Record arm failures and deadline misses separately. A histogram alone cannot
  prove reliability.

Then validate and render it with the same run:

```sh
python3 tests/tooling/zephyr_opt_dashboard.py collect \
  --build build/cdk-opt-latency \
  --latency-log internal/zephyr-opt/captures/control.log \
  --snapshot internal/zephyr-opt/captures/control-runtime.json \
  --attempts 1000 --warmup 20 \
  --profile thread+lto --experiment control
```

Labels are deliberately restricted. Secret-shaped text, hardware addresses,
long hex values, and personal home paths are rejected or redacted before any
artifact is written.

## Read and compare runs

Open `internal/zephyr-opt/dashboard/index.html` directly with `file://` or serve
the directory from a local-only HTTP server. `latest.json` is the stable agent
entry point. Each collection also creates:

```text
runs/<run-id>/manifest.json
runs/<run-id>/metrics.json
runs/<run-id>/commands.txt
runs/<run-id>/artifacts/*
```

Use `--control-run <run-id>` to record pairing metadata for an A/B experiment.
The dashboard displays one run at a time. Agents compare exact values from the
two `metrics.json` files only when their configurations match.

Re-render a preserved model without collecting anything:

```sh
python3 tests/tooling/zephyr_opt_dashboard.py render \
  --input internal/zephyr-opt/dashboard/latest.json
```

Run the tooling contract tests with:

```sh
python3 -m unittest tests/tooling/test_zephyr_opt_dashboard.py -v
```
