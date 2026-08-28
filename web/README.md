# web/

The site, the browser flasher, the digital twin, and the subsystem graph.

```sh
python3 web/build.py --check      # build web/dist/, fail on any dead link
```

Stdlib Python only. No node, no bundler, nothing to install.

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
source. Pages opt in with markers the build substitutes:

| Marker | Gets |
|---|---|
| `@@HEAD@@` | meta, canonical, Open Graph, favicon, font preloads, stylesheet |
| `@@NAV@@` | the site topbar, with `aria-current` on the page you are on |
| `@@TOOLBAR:crumb@@` | the narrower bar the twin, flasher and graph share |
| `@@FOOTER@@` | the footer, and the deferred `site.js` |
| `@@SITEJS@@` | just the script, for full-viewport pages that have no footer |

Links are relative and the depth is computed per page, so the site works
unchanged at a domain root or under a project subpath. The Graph link is emitted
only when the graph was actually built; the link gate excuses that path, so
nothing else would catch a nav item that 404s.

## Gates

`--check` fails the build on any of these.

| Gate | Checks |
|---|---|
| link gate | every relative `href`/`src` in the output resolves |
| `twin/check_constants.py` | the twin's `FW` table still matches the C it cites |
| `site/check_hero_constants.py` | the landing hero's tick rate and unlock bound ditto |
| `flasher/check_codes.py` | the setup code, QR payload and its provenance hash |

Both constant gates share one convention: `NAME: value, // path:line`. The
format is load-bearing, since each gate re-reads the cited line and fails if the
value has moved off it. **Do not reformat those tables.**

## The graph's data

Three data sources; which ones you have decide which version of the page you get.

| Source | Size | In git | Gives you |
|---|---|---|---|
| `graph/subsystems.json` | 4 KB | yes | the flat SVG graph, always |
| `graph/files.json` | 680 KB | yes | the 3D file-level graph, with `vendor/` |
| `graphify-out/graph.json` | 11 MB | no | the same 3D page, from fresher data |

`graphify update .` writes the big one: 7,969 nodes and 18,457 edges, which this
page reduces to 393 files with their symbols and 703 links, or to 17 subsystems
and 49 edges. The repository carries both reductions and leaves the 11 MB where
graphify put it, which is what makes the published site the flyable page.

Refreshing the distillates is `make docs-graph-refresh`, a deliberate step and
never a side effect of a build: its first line is the commit the graph was
extracted at, so a build that rewrote it would dirty every worktree with
graphify data and conflict between any two branches that had built. Run it, read
the diff, commit it on its own. Both files are sorted and one entry per line, so
a reviewer can see that a subsystem gained four files.

The 3D page still needs a renderer that is fetched, not committed:
`make docs-graph3d` locally, one step in `.github/workflows/pages.yml` for the
deploy. Without it the flat page builds, which is a fallback and not a degraded
mode.

## Two rules

**Nothing generated is committed.** `dist/` is gitignored, and the fix for a
stale site is to build it again. Same reason `twin.js` is not in the tree: it is
36 KB of minified emscripten on one line, built from `twin/twin_glue.c` when
emscripten is present, and the twin page says so plainly when it is not.

**Nothing outside this repository.** Everything the site needs is in this
directory, so a fresh clone and CI build the same thing a contributor does.

## Design system

`assets/design/` is vendored from the UltraWideLock v2 design system: mint on
deep teal, Space Grotesk and JetBrains Mono, dark canonical with a light theme
that holds AA. Token and class names are unchanged from that source, so it can
be re-vendored by copying files over.

Two signals, not one accent. Mint is the first path: direct, line-of-sight,
trusted. Amber is the late path: obstructed, or a relay's added delay. That is
the classifier the firmware ships, so the pair means the same thing on the
landing hero, in the twin and in the guides. The aliases are `--path-first` and
`--path-late` in `tokens/colors.css`.

Fonts are self-hosted WOFF2, declared by `tokens/typography.css`. The one
external subresource on the whole site is `esp-web-tools` on the flasher page,
pinned to an exact version with an SRI hash and constrained by that page's CSP.

The source files are split for authoring and concatenated into a single
`styles.css` at build time. **Do not add an `@import` between them**: each one
is a serial round trip the browser cannot discover until the parent sheet has
arrived.
