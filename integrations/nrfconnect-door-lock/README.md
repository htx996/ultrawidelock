# Nordic door-lock integration

This integration adapts Nordic's door-lock and access-control application for
UltraWideLock on the nRF5340 DK. Three directories, and the difference between
them is what this repository owns.

| | |
| --- | --- |
| [`app/`](app/) | The application itself, ours to edit. Nordic's `matter-aliro-door-lock-app` at the pinned revision plus our changes, redistributed under `LicenseRef-Nordic-5-Clause`. [`app/UPSTREAM.md`](app/UPSTREAM.md) records where it came from. |
| [`patches/`](patches/) | Four patches into the fetched workspace, for code that is not ours to move: the add-on's Aliro subsystem, NCS's shared Matter sample sources, and connectedhomeip's Zephyr BLE platform. |
| [`tooling-patches/`](tooling-patches/) | Not applied by any build. Fixes to the ZAP editor scripts, for whoever next edits the Matter data model by hand. |

The add-on, NCS, Zephyr and Matter are still fetched and pinned by `make
bootstrap`, into the machine's workspace store (`scripts/lib/ws.sh`), and every
checkout on the same patch set links to one tree.

There were fifteen patches. Eleven of them changed the application, and became
`app/`; the reasoning is in `app/UPSTREAM.md`, and the shortest version is that
a patch cannot be read, edited, or opened in an editor -- only applied. The
Home Assistant data model came with them: `HA=1` now sets
`CONFIG_ULTRAWIDELOCK_HA`, which selects `app/src/matter/zap_uwb_ha` in place of
`app/src/matter/zap_uwb`, where it used to be two patches that had to be applied
in one order and no other.

`bootstrap.sh` resets the fetched trees to their pinned revisions before
applying what is left, and records a fingerprint of the patch contents. The
product build rejects a workspace carrying a different one -- a check with
almost nothing left to catch, since the store names a tree after that same
fingerprint.

The product-owned launcher and overlays live in
[`apps/nrf5340dk-lock/`](../../apps/nrf5340dk-lock/). Build the integrated
product with:

```sh
make bootstrap        # once per machine
make ws-link          # once per checkout after that
make nrf-build
```

Two things to run when raising the pin in `scripts/bootstrap.sh`:

```sh
tests/tooling/patch_drift_check.sh   # do the four patches still apply?
scripts/app-upstream-diff.sh         # what did we change in app/, and did upstream touch it?
```
