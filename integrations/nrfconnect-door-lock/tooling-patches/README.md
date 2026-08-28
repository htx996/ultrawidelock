# Tooling patches

Patches for the workspace that **no firmware build applies**. Nothing in
`scripts/bootstrap.sh` or `scripts/ws-link.sh` reads this directory, and the
workspace store's entry name is not computed from it: what is here changes a
developer command on this machine, never a byte of the image.

That is why the directory exists separately. The patches in `../patches/` are
part of what the image *is*, so a workspace carrying a different set of them
gets its own entry in the store. Folding a hand-applied ZAP script fix into that
set would split the store for no reason.

## `zap-tooling.patch`

Four fixes to `modules/lib/matter/scripts/west/zap_*.py`, behind `west zap-gui`
and `west zap-sync`:

| File | What it fixes |
| --- | --- |
| `zap_append.py` | An attribute whose name is element text rather than a `name=` attribute read as `None`, so custom types went unreported. |
| `zap_common.py` | The ZAP subprocess's stdout and stderr were discarded on the error path, which is where they are worth reading. |
| `zap_gui.py`, `zap_sync.py` | Chromium's sandbox aborts with no output at all; that now raises rather than being read as success. `zap-sync -j` pointed at the SDK's own `zcl.json` deleted it and then failed to copy it back onto itself. |

Apply it only when about to edit the Matter data model:

```sh
git -C workspace/modules/lib/matter apply \
    integrations/nrfconnect-door-lock/tooling-patches/zap-tooling.patch
```

The workspace is shared with every checkout on the same patch set, so this
dirties a tree that is not only yours. Nothing is lost: the next `make ws-link`
or `make bootstrap` resets the repository and saves what it discarded under
`.ultrawidelock-discarded/` in the workspace.

`tests/tooling/patch_drift_check.sh` does not cover this one, only the patches
that build the firmware. If it stops applying, the ZAP editor tells you.
