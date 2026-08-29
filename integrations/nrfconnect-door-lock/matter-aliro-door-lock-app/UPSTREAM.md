# Where this application came from

This directory is Nordic's `applications/matter-aliro-door-lock-app`, taken
from [nrfconnect/ncs-door-lock-and-access-control][repo] and carried in this
repository as source we own and change directly.

| | |
| --- | --- |
| Upstream repository | `https://github.com/nrfconnect/ncs-door-lock-and-access-control` |
| Upstream path | `applications/matter-aliro-door-lock-app` |
| Revision | `a5ad7fde1041d81690710a949c98eda1985fee0b` |
| License | `LicenseRef-Nordic-5-Clause`: see `THIRD_PARTY_NOTICES` |

The revision is not repeated here as a separate fact: it is the `PIN` in
`scripts/bootstrap.sh`, and `scripts/app-upstream-diff.sh` reads it from there.
The table says it out loud for a reader, and that script is what would notice if
the two stopped agreeing.

## Why it is here and not patched into the workspace

It was eleven patch files applied to the fetched tree by every bootstrap:
`approach-direction-cluster`, `console-quiet-flood`, `cred-doc-time-ratchet`,
`cred-shell-factoryreset`, `cred-time-persist`, `crypto-timesync-tap`,
`kpersistent-orphan-selfheal`, `nfc-transport-seam`, `pretty-shell`, and the two
Home Assistant data-model patches.

Patching a fetched tree costs three things this does not. The application could
only be read in its patched form by a build, never by a reader or an editor. A
change to it meant editing a diff by hand, or editing the workspace and
re-cutting one, which is how the rename of 2026-08 rewrote upstream context
lines in all twelve patches and left every hunk unable to apply. And the two
Home Assistant patches were a pair that had to be applied in one order and no
other, because the second was cut against a tree with the first already on it --
an invariant that lived in a comment.

None of that survives ownership. `HA=1` selects `src/matter/zap_uwb_ha` instead
of `src/matter/zap_uwb` (`CONFIG_ULTRAWIDELOCK_HA`), which is a directory rather
than an ordering.

## What replaces the drift check

`tests/tooling/patch_drift_check.sh` answers "do our patches still apply to the
pinned upstream", and it cannot answer anything about owned source: there is no
apply to fail. `scripts/app-upstream-diff.sh` answers the question that is left
-- how this directory differs from the upstream revision above -- by fetching
just that path at that revision and diffing. It reports; it does not gate. Our
changes are supposed to be there.

What it is really for is a pin bump. Raising `PIN` without looking at that diff
means merging by hope, and the diff is the list of everything a new upstream
revision has to be reconciled against.

[repo]: https://github.com/nrfconnect/ncs-door-lock-and-access-control
