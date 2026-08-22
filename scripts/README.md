# Scripts

`scripts/` contains command-line helpers for top-level Make targets, device
operations, and release workflows.

| Group | Scripts |
|---|---|
| Setup and environment | `bootstrap.sh`, `toolchain.sh`, `check-signing-key.sh` |
| nRF5340 DK builds | `nrf5340dk-build.sh` |
| DWM3001CDK operations | `cdk-dfu.sh`, `cdk-find-probe.sh`, `cdk-rtt-elf-check.sh` |
| Firmware size | `cdk-size.py`, `cdk-size-compare.py`, `cdk-size-baseline.py` |
| Delta update and SMP | `ultrawidelock_patch.py`, `ultrawidelock_push.py`, `ultrawidelock_smp.py` |
| Provisioning | `ultrawidelock-enroll.py`, `spake2p_verifier.py` |
| Release and validation | `release-bundle.sh`, `hitl-run.sh`, `test-runner.sh` |
| Shared shell library | `lib/ui.sh` |

`lib/ui.sh` is sourced, not run: it is the progress display behind the Make
targets that take minutes (`make test`, `make cbmc`, `make check`). On a
terminal it draws a step counter, a percentage, a bar and the elapsed time; in a
pipe, a file or CI it prints one line per step and no escape sequences, and the
wrapped command's own output goes to stdout untouched either way. Set
`ULTRAWIDELOCK_UI=0` to force the plain form, `1` to force the drawn one, and
run `scripts/lib/ui.sh --self-test` to check it against a terminal that is
missing colour, UTF-8, width or `$TERM`.

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
