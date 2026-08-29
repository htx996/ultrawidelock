# Tests

| Directory | Scope |
|---|---|
| `host/` | native unit tests, fakes, sanitizers, coverage, and CBMC harnesses |
| `shared/` | portable tests compiled by more than one environment |
| `ports/` | framework-port host verification |
| `tooling/` | purity, drift, source-role, patch, seam, and static-analysis gates |
| `sdk/` | installed CMake package and external C consumer |
| `on_target/` | hardware-backed Zephyr and ESP32 tests |

`make check` runs the whole host-side gate: 18 suites, 9,608 checks. For a
narrower surface use `make test`, `make sdk-check`, `make test-san`,
`make coverage`, `make cbmc`, `make drift`, `make seam`, `make purity`,
`make lint` or `make test-twin`.

## The three tiers

Each tier is a superset of the one above it. Pick the cheapest that can see the
change you made.

| Command | Needs | Covers | Cost |
|---|---|---|---|
| `make check` | a C compiler and python3 | every host suite; what CI judges, via `make ci` | ~2 min |
| `make regress` | the NCS toolchain, `./workspace`, a signing key | the above, plus the network and emscripten suites, every DWM3001CDK configuration and the size gate | ~15 min |
| `make regress-hil` | a reader on its probe and the DK as the phone | the above, plus the walk-up loop on air | ~25 min |

CI runs the first tier only (`.github/workflows/ci.yml`): the NCS toolchain and
its multi-GB workspace are more than a hosted runner should carry.
`make regress` is the pre-push gate for anything reaching Kconfig, devicetree, a
linker script or flash fit. `make regress-hil` writes
`build/regress-hil/<timestamp>/verdict.txt`, mapping each stage to its row in
`docs/hardware-validation.md`; rows CDK-9, CDK-10 and CDK-14..CDK-18 are still
manual.

### Keeping the size baseline honest

`make cdk-size-check` compares the built image against
`apps/dwm3001cdk-lock/size-baseline.json`, and can only do that while the
baseline describes the same configuration. When it does not it exits 3, "not
comparable", and `fw-regress` fails: a size gate that cannot compare must be as
loud as one that fails. The baseline went not-comparable at the Kconfig prefix
rename and stayed there for 93 firmware commits, quietly absorbing about 17 KB
of flash growth.

Refresh it after a merge to main, not on a feature branch:

```
make regress                 # builds every configuration and gates size
make cdk-size-baseline       # re-record from the matter build
make cdk-size-baseline CDK_BUILD=$ULTRAWIDELOCK_BUILD_ROOT/cdk-shipping
```

Both entries are recorded; the shipping one is primary. `fw-regress` ends with
`cdk-size-age`, which prints how many firmware-touching commits the baseline is
behind HEAD and says so loudly past `CDK_SIZE_AGE_WARN` (25). It warns rather
than fails, because a feature branch is legitimately ahead of the baseline.

`patchdrift` runs from `make regress` rather than the default set: it fetches
from public GitHub, and the default set has to pass offline. The `twin` suite is
in the default set and skips loudly without the emscripten SDK.

A stage whose compile fails fails the run. Stages run in parallel, and one
invoked as `"$fn" || rc=$?` loses `errexit` to bash's condition context, so a
failed compile once fell through and ran the *previous* binary. Each stage now
runs in its own `(set -e; ...)` subshell whose real status `wait` reports.

## Static analysis

Four passes read the portable tree, each catching what the others cannot.

| Command | Tool | Reads | Cost |
|---|---|---|---|
| `make lint` | cppcheck | patterns, every path whether a test reaches it or not | seconds |
| `make test-san` | ASan + UBSan | memory behaviour, but only on the paths a suite exercises | ~1 min |
| `make cbmc` | CBMC | the wire parsers exhaustively, within bounds | minutes |
| `make sca` | Clang Static Analyzer | values followed across functions and branches | seconds |

`make lint` runs inside `make check` and CI. `make sca` does not: CodeChecker is
a Python package rather than a one-line install. Run it before a release, or when
touching parsing and session code:

```sh
python3 -m venv .venv-sca
.venv-sca/bin/pip install codechecker
CODECHECKER=.venv-sca/bin/CodeChecker make sca
```

Both gates cover `modules/` and `include/` only: `ports/` and `apps/` need Zephyr
and ESP-IDF to expand their macros, so either tool aimed at them reports missing
SDK headers rather than defects. Each takes a `--self-test` flag that plants a bug
of the class it exists to catch.
