# web/

The site, the browser flasher, the digital twin, and the subsystem graph.

```sh
python3 web/build.py --check      # build web/dist/, fail on any dead link
```

Stdlib Python only. No node, no bundler, nothing to install.

## What is here

| Path | What it is |
|---|---|
| `build.py` | the whole generator: page shell, asset bundle, link gate, drift gates |
| `site/` | landing and docs page templates, the Markdown renderer |
| `flasher/` | install over a cable (ESP32) and update over Bluetooth or USB (both boards) |
| `twin/` | the walk-up digital twin, firmware logic compiled to WASM |
| `graph/` | subsystem graph, generated from the source tree |
| `assets/design/` | vendored design system: tokens, components, scripts |
| `dist/` | **generated, gitignored, disposable** |

## The shell

Every page's `<head>`, topbar and footer come from `build.py`, not from the page
source. Before that they existed in four hand-written versions that had already
drifted apart: the landing page had a theme toggle, the docs template had none,
the twin persisted its choice under a different `localStorage` key so it never
inherited the site's, and the 3D graph had no toggle at all.

Pages opt in with markers the build substitutes:

| Marker | Gets |
|---|---|
| `@@HEAD@@` | meta, canonical, Open Graph, favicon, font preloads, stylesheet |
| `@@NAV@@` | the site topbar, with `aria-current` on the page you are on |
| `@@TOOLBAR:crumb@@` | the narrower bar the twin, flasher and graph share |
| `@@FOOTER@@` | the footer, and the deferred `site.js` |
| `@@SITEJS@@` | just the script, for full-viewport pages that have no footer |

Links are relative and the depth is computed per page, so the site works
unchanged at a domain root or under a project subpath.

The Graph link is emitted only when the graph was actually built. It needs
graphify, which is not in this repository, so linking it unconditionally
published a nav item that 404'd on every build without `graphify-out/` — and
the link gate deliberately excuses that path, so nothing caught it.

## Gates

`--check` fails the build on any of these. They are wired in because all three
existed already and nothing ran them, which meant all three had quietly rotted
into scripts that could not have passed.

| Gate | Checks |
|---|---|
| link gate | every relative `href`/`src` in the output resolves |
| `twin/check_constants.py` | the twin's `FW` table still matches the C it cites |
| `site/check_hero_constants.py` | the landing hero's tick rate and unlock bound ditto |
| `flasher/check_codes.py` | the setup code, QR payload and its provenance hash |

The two constant gates share one convention: `NAME: value, // path:line`. The
format is load-bearing — a gate re-reads each cited line and fails if the value
has moved off it. Do not reformat those tables.

## The graph's data

The subsystem graph has three data sources, and which ones you have decide which
version of the page you get.

| Source | Size | In git | Gives you |
|---|---|---|---|
| `graph/subsystems.json` | 4 KB | yes | the flat SVG graph, always |
| `graph/files.json` | 680 KB | yes | the 3D file-level graph, with `vendor/` |
| `graphify-out/graph.json` | 11 MB | no | the same 3D page, from fresher data |

`graphify update .` writes the big one. It is 7,969 nodes and 18,457 edges, and
this page reduces all of that to 393 files with their symbols and 703 links, or
to 17 subsystems and 49 edges — so the repository carries both reductions and
leaves the 11 MB where graphify put it.

Carrying `files.json` is what makes the published site the flyable page. Before
it, CI had neither the graphify data nor the renderer, so github.io shipped the
flat SVG on every deploy while every developer machine showed the 3D one.

Refreshing the distillates is a deliberate step, `make docs-graph-refresh`, and
never a side effect of an ordinary build. Its first line is the commit the graph
was extracted at, so a build that rewrote it left a dirty tree in every worktree
that had graphify data, and any two branches that had both built then conflicted
on that line. Run the refresh, read the diff, commit it on its own: one sorted
line per subsystem, one per edge, and a reviewer can see that a subsystem gained
four files or that a new dependency appeared.

That is deliberately not the `twin.js` mistake. Both files are sorted,
line-per-entry and regenerate deterministically; `twin.js` was 36 KB of
minified emscripten on one line. `files.json` is 680 KB, which is large for a
committed artifact and is the price of the published page being the real one —
it is one field per line, so a diff still reads. Committing them is what lets
the graph page build from a fresh clone and in CI, which is the second rule
below.

The 3D page still needs a renderer that is fetched, not committed:
`make docs-graph3d` locally, one step in `.github/workflows/pages.yml` for the
deploy. Without it the flat page builds, which is a fallback and not a degraded
mode.

## Two rules

**Nothing generated is committed.** The pipeline this replaces wrote one page
per source file into `docs/` and committed all 476 of them. Those pages carried
derived line numbers, so every merge conflicted on lines no resolution could
settle correctly. It needed a `make sync` target whose own comment admitted
that "only a regeneration can" fix it. `dist/` is gitignored, and the fix for a
stale site is to build it again.

The same rule is why `twin.js` is not in the tree. It was a 36 KB minified
emscripten bundle on a single line, committed: the worst possible merge
conflict. It is built from `twin/twin_glue.c` when emscripten is present, and
the twin page says so plainly when it is not.

**Nothing outside this repository.** The old pipeline searched machine-local
paths for a page generator that lived somewhere else, so a fresh clone could
not build the site at all, and CI built something no contributor could
reproduce. Everything needed is in this directory.

## Design system

`assets/design/` is vendored from the UltraWideLock v2 design system: mint on
deep teal, Space Grotesk and JetBrains Mono, dark canonical with a light theme
that holds AA. Token and class names are unchanged from that source, so it can
be re-vendored by copying files over.

Two signals, not one accent. Mint is the first path — direct, line-of-sight,
trusted. Amber is the late path — obstructed, or a relay's added delay. That is
the classifier the firmware actually ships, so the pair means the same thing on
the landing hero, in the twin and in the guides. The aliases are
`--path-first` and `--path-late` in `tokens/colors.css`.

Fonts are self-hosted WOFF2 in `fonts/`, declared by `tokens/typography.css`.
Nothing here reaches a third party. The one external subresource on the whole
site is `esp-web-tools` on the flasher page, pinned to an exact version with an
SRI hash and constrained by that page's CSP; the reasoning is in the comment
above the script tag.

The source files are split for authoring and concatenated into a single
`styles.css` at build time. Do not add an `@import` between them: each one is a
serial round trip the browser cannot discover until the parent sheet has
already arrived.
