# Scripts

`scripts/` contains command-line helpers for top-level Make targets, device
operations, and release workflows.

| Group | Scripts |
|---|---|
| Setup and environment | `bootstrap.sh`, `esp-bootstrap.sh`, `toolchain.sh`, `check-signing-key.sh`, `ws-link.sh`, `ws-store.sh` |
| nRF5340 DK builds | `nrf5340dk-build.sh`, `app-upstream-diff.sh` |
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

`app-upstream-diff.sh` says how our copy of the door-lock application differs
from the Nordic application it was taken from, by fetching just that path at the
pin and diffing. It reports rather than gates: the changes are supposed to be
there. It is the thing to read before raising `PIN`, which is the one moment the
answer decides anything, and it is what replaced eleven patch files as the
record of what this repository changed in that application.

`ws-link.sh` points a checkout at the workspace its branch needs. The machine
keeps one store of them (`lib/ws.sh`), a tree per pin and patch set, so a
checkout that agrees with one already there is a symlink and nothing else; a
branch carrying its own patch set gets a copy-on-write clone of the nearest
entry and a re-patch, not a re-fetch. `make ws-link` links the checkout it runs
in. Pass a path to link a different one -- the way to reach a worktree whose
branch predates the script, since it needs no files copied into the target and
no commit on its branch. `make ws-store` lists what the machine is holding and
which checkouts still link to it, because a shared tree outlives the worktree
that fetched it.

`ws-link.sh --print` names the entry a checkout resolves to and changes nothing.
It needs no workspace, no toolchain and no network -- the name is a function of
the pin, the NCS version and the patch files, all of which a bare checkout has
-- so it answers in milliseconds on a cold machine. That makes it the cache key
for a build runner: restore the store entry under that name, `make ws-link`, and
a build that would have refetched for an hour starts immediately. Restoring by
the fetch-key prefix alone is also safe, but only because the restored tree
keeps its own name and `ws-link` clones and re-patches it into the right one.
The clone is free where the filesystem has block cloning and a real 5.5 GB copy
where it does not; both beat a fetch.

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
