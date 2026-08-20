# Tests

`tests/` verifies the portable implementation, platform boundaries, and target
integration surfaces.

| Directory | Scope |
|---|---|
| `host/` | Native unit tests, fakes, sanitizers, coverage, and CBMC harnesses |
| `shared/` | Portable tests compiled by more than one environment |
| `ports/` | Framework-port host verification |
| `tooling/` | Purity, drift, source-role, patch, seam, and static-analysis gates |
| `sdk/` | Installed CMake package and external C consumer |
| `on_target/` | Hardware-backed Zephyr and ESP32 tests |

Run the complete host-side gate with:

```sh
make check
```

Use `make test`, `make sdk-check`, `make test-san`, `make coverage`, `make cbmc`,
`make drift`, `make seam`, `make purity`, `make lint`, or `make test-twin` for a
narrower surface.

## The three tiers

Each tier is a superset of the one above it, and each costs more to run. Pick the
cheapest one that can see the change you made.

| Command | Needs | Covers | Cost |
|---|---|---|---|
| `make check` | a C compiler and python3 | every host suite; what CI judges, via `make ci` | ~2 min |
| `make regress` | the NCS toolchain, `./workspace`, a signing key | the above, plus the network and emscripten suites, plus every DWM3001CDK configuration and the size gate | ~15 min |
| `make regress-hil` | a reader on its probe and the DK as the phone | the above, plus the walk-up loop on air | ~25 min |

CI runs the first tier only, and says why in `.github/workflows/ci.yml`: the NCS
toolchain and its multi-GB workspace are more than a hosted runner should carry.
`make regress` is the pre-push gate for anything that reaches Kconfig, devicetree,
a linker script or flash fit, none of which a host suite can see. `make regress-hil`
writes `build/regress-hil/<timestamp>/verdict.txt`, mapping each stage to its row
in `docs/hardware-validation.md`; rows CDK-9, CDK-10 and CDK-14..CDK-18 are still
manual.

### Keeping the size baseline honest

`make cdk-size-check` compares the built image against
`apps/dwm3001cdk-lock/size-baseline.json`, and it can only do that while the
baseline still describes the same configuration. When it does not, it exits 3,
"not comparable", and `fw-regress` fails: a size gate that cannot compare must be
as loud as one that fails, because the alternative is what already happened here.
The baseline went not-comparable at the Aliro rename and stayed there for 93
firmware commits, quietly absorbing about 17 KB of flash growth.

So refresh it after a merge to main, not on a feature branch:

```
make regress                 # builds every configuration and gates size
make cdk-size-baseline       # re-record from the matter build
make cdk-size-baseline CDK_BUILD=$ULTRAWIDELOCK_BUILD_ROOT/cdk-shipping
```

Both entries are recorded, and the shipping one is what the gate treats as
primary. `fw-regress` ends with `cdk-size-age`, which prints how many
firmware-touching commits the baseline is behind HEAD and says so loudly past
`CDK_SIZE_AGE_WARN` (25). It warns rather than fails, because a feature branch is
legitimately ahead of the baseline and a gate that cries on every branch is one
people learn to ignore.

One suite is registered but out of the default set, and runs from `make regress`
instead: `patchdrift` fetches from public GitHub, so it cannot be in a set that has
to pass offline. The `twin` suite is in the default set and needs the emscripten
SDK, which it skips loudly for rather than passing quietly, so a machine without
emscripten still gets an honest verdict on everything else.

## Static analysis

Four passes read the portable tree, each catching what the others cannot:

| Command | Tool | Reads | Cost |
|---|---|---|---|
| `make lint` | cppcheck | patterns, every path whether a test reaches it or not | seconds |
| `make test-san` | ASan + UBSan | memory behaviour, but only on the paths a suite exercises | ~1 min |
| `make cbmc` | CBMC | the wire parsers exhaustively, within bounds | minutes |
| `make sca` | Clang Static Analyzer | values followed across functions and branches | seconds |

`make lint` runs inside `make check` and CI. `make sca` does not: CodeChecker is
a Python package rather than a one-line install, so requiring it would fail a
clean checkout for a reason unrelated to the change under test. Run it before a
release, or when touching parsing and session code:

```sh
python3 -m venv .venv-sca
.venv-sca/bin/pip install codechecker
CODECHECKER=.venv-sca/bin/CodeChecker make sca
```

Both gates cover `modules/` and `include/` only. `ports/` and `apps/` cannot be
parsed without Zephyr and ESP-IDF expanding their macros, so pointing either
tool at them reports missing SDK headers rather than defects. Each gate takes a
`--self-test` flag that plants a bug of the class it exists to catch.
