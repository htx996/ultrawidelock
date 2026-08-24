# Scripts

`scripts/` contains command-line helpers for top-level Make targets, device
operations, and release workflows.

| Group | Scripts |
|---|---|
| Setup and environment | `bootstrap.sh`, `esp-bootstrap.sh`, `toolchain.sh`, `check-signing-key.sh`, `ws-seed.sh` |
| nRF5340 DK builds | `nrf5340dk-build.sh` |
| DWM3001CDK operations | `cdk-dfu.sh`, `cdk-find-probe.sh`, `cdk-rtt-elf-check.sh` |
| Firmware size | `cdk-size.py`, `cdk-size-compare.py`, `cdk-size-baseline.py` |
| Delta update and SMP | `ultrawidelock_patch.py`, `ultrawidelock_push.py`, `ultrawidelock_smp.py` |
| Provisioning | `ultrawidelock-enroll.py`, `spake2p_verifier.py` |
| Release and validation | `release-bundle.sh`, `hitl-run.sh`, `test-runner.sh` |
| Shared shell library | `lib/ui.sh`, `lib/setup.sh` |

`lib/ui.sh` is sourced, not run: it is the progress display behind the Make
targets that take minutes (`make test`, `make cbmc`, `make check`). On a
terminal it draws a step counter, a percentage, a bar and the elapsed time; in a
pipe, a file or CI it prints one line per step and no escape sequences, and the
wrapped command's own output goes to stdout untouched either way. Set
`ULTRAWIDELOCK_UI=0` to force the plain form, `1` to force the drawn one, and
run `scripts/lib/ui.sh --self-test` to check it against a terminal that is
missing colour, UTF-8, width or `$TERM`.

`bootstrap.sh` (NCS, for both Zephyr ports) and `esp-bootstrap.sh` (ESP-IDF and
esp-matter) both source `lib/setup.sh`, which is the reason they stop, ask and
resume identically: it owns the phase output, the `die` format, the traps that
keep an interrupt legible and a `set -e` abort nonzero, `ask`/`SETUP_AUTO`, the
per-host package hints and the disk and network checks. Neither script knows
anything about the other's SDK.

`ws-seed.sh` gives a worktree its own west workspace as an APFS copy-on-write
clone, so branch-bouncing cannot build stale patches. `make ws-seed` seeds the
worktree it runs in. Pass a path to seed a different one -- the way to reach a
worktree whose branch predates the script, since it needs no files copied into
the target and no commit on its branch. The target must have an executable
`scripts/bootstrap.sh`, which is re-run inside it to normalize patches to its
own branch, and is refused up front when it does not.

Prefer a documented Make target when one exists. Run `make help` to see the
supported interface and required variables. Use `make hitl` for `hitl-run.sh`;
pass its optional flags through `HITL_ARGS`.

The native BLE delta-update protocol is version 2. Every request carries a
nonzero transfer ID, DATA also carries its absolute offset, and each successful
reply echoes the transfer ID plus the receiver's next offset. Retrying an
unchanged frame after a lost notification is therefore safe. Version-2 request
opcodes are `0x11` through `0x14`; they intentionally do not overlap the old
transfer-blind protocol, so a mixed host and firmware pair fails loudly.

Error 8, "another update transport owns the receiver", means the SMP half or an
earlier BLE session still holds the claim. It clears on disconnect or when the
update window closes; it is not a signature or a corruption failure.

`cdk-dfu.sh` no longer resets the board over SWD, because the bootloader no
longer waits for mcumgr on every boot. Its operator step is now a **>= 5 s SW2
hold while the application is running**, which requests MCUboot serial recovery
and warm-reboots into it. Its fourth argument, the chip name, is vestigial.
