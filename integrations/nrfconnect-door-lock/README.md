# Nordic door-lock integration

Nordic's door-lock and access-control application, adapted for UltraWideLock on
the nRF5340 DK. Three directories, differing in what this repository owns.

| | |
| --- | --- |
| [`matter-aliro-door-lock-app/`](matter-aliro-door-lock-app/) | The application, ours to edit: Nordic's `matter-aliro-door-lock-app` at the pinned revision plus our changes, redistributed under `LicenseRef-Nordic-5-Clause`. [`UPSTREAM.md`](matter-aliro-door-lock-app/UPSTREAM.md) records where it came from. |
| [`patches/`](patches/) | Four patches into the fetched workspace, for code that is not ours to move: the add-on's Aliro subsystem, NCS's shared Matter sample sources, and connectedhomeip's Zephyr BLE platform. |
| [`tooling-patches/`](tooling-patches/) | Not applied by any build. Fixes to the ZAP editor scripts, for whoever next edits the Matter data model by hand. |

The add-on, NCS, Zephyr and Matter are fetched and pinned by `make bootstrap`
into the machine's workspace store (`scripts/lib/ws.sh`), and every checkout on
the same patch set links to one tree.

There were fifteen patches. Eleven changed the application and became
`matter-aliro-door-lock-app/`, because a patch cannot be read, edited or opened
in an editor, only applied; `UPSTREAM.md` has the reasoning. The Home Assistant
data model came with them: `HA=1` now sets `CONFIG_ULTRAWIDELOCK_HA`, which
selects `src/matter/zap_uwb_ha` in place of `src/matter/zap_uwb`, where it used
to be two patches that had to be applied in one order and no other.

`bootstrap.sh` resets the fetched trees to their pinned revisions before applying
what is left, and records a fingerprint of the patch contents. The product build
rejects a workspace carrying a different one.

The product-owned launcher and overlays live in
[`apps/nrf5340dk-lock/`](../../apps/nrf5340dk-lock/):

```sh
make bootstrap        # once per machine
make ws-link          # once per checkout after that
make nrf-build
```

Two things to run when raising the pin in `scripts/bootstrap.sh`:

```sh
tests/tooling/patch_drift_check.sh   # do the four patches still apply?
scripts/app-upstream-diff.sh         # what did we change, and did upstream touch it?
```
