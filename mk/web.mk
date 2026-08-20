# mk/web.mk — the site: landing page, guides, flasher, twin, subsystem graph.
#
# This is not the `make docs` that was removed. That one shelled out to a page
# generator living outside the repository, wrote a page per source file into
# docs/, and committed all 476 of them; every merge then conflicted on derived
# line numbers, which is why it needed a `make sync` whose own comment admitted
# only a regeneration could fix them.
#
# Nothing here is committed. web/dist/ is gitignored and disposable, the
# generator is stdlib Python in this repository, and the whole site builds with
# no toolchain, no node and nothing installed.
#
# Four optional inputs, each degrading rather than failing:
#   emcc                                  the twin's simulator
#   graphify-out/graph.json               fresher file-level graph data; without
#                                         it both graph pages still build, from
#                                         the committed web/graph/subsystems.json
#                                         and web/graph/files.json
#   web/vendor/3d-force-graph.min.js      3D rather than the flat SVG graph
#   build/esp32-matter-lock-*/*.bin       the flasher's images; without them
#                                         the page says so instead of offering
#                                         a button that downloads nothing

.PHONY: docs docs-check docs-serve docs-clean docs-deps docs-graph3d \
        docs-graph-refresh release-all test-twin

# ---------------------------------------------------------------------------
# Auto-provisioning, used by `docs` and deliberately NOT by `docs-check`.
#
# `docs` is what a person runs, and it should produce the best site this
# machine can make -- so it offers to fetch the two things the repository does
# not carry. `docs-check` is the gate CI runs. It stays hermetic and offline:
# a gate that reaches the network can fail for reasons that have nothing to do
# with the change being gated, and CI has no terminal to answer a prompt.
#
# Nothing is installed or downloaded without being asked first, and declining
# is a normal answer -- build.py degrades every one of these to something
# honest. Two escape hatches:
#
#   DOCS_AUTO=1   yes to everything, no prompts   (scripts, first-run setup)
#   DOCS_AUTO=0   no to everything, no prompts    (offline, or just quiet)
#
# With no answer possible (stdin is not a terminal) the default is no.
DOCS_AUTO ?=
GRAPHIFY_VENV := $(REPO_ROOT)/.venv-graphify
# The CLI is `graphify`; the distribution on PyPI is `graphifyy`. Pinned rather
# than floating, because it decides what the published graph page contains.
GRAPHIFY_PKG ?= graphifyy==0.9.36
GRAPH3D_URL ?= https://unpkg.com/3d-force-graph@1/dist/3d-force-graph.min.js

##@ Docs
## docs: build the website  ->  web/dist/index.html
##   Landing page, the guides under docs/, the flasher, the twin and the graph.
##   Offers to fetch the 3D graph renderer and to install/refresh graphify.
##   DOCS_AUTO=1 to accept without asking, DOCS_AUTO=0 to skip it all.
docs: docs-deps
	@python3 $(REPO_ROOT)/web/build.py

## docs-check: build the website and fail on any dead internal link
##   Hermetic: never fetches, never installs, never prompts. This is the CI gate.
docs-check:
	@python3 $(REPO_ROOT)/web/build.py --check

## docs-graph-refresh: rewrite the committed graph data from graphify-out/
##   Two files, both written by this target: the 4 KB web/graph/subsystems.json
##   the flat page draws, and the 680 KB web/graph/files.json the 3D page flies
##   through. They are the only graph data a fresh clone or CI has, so they have
##   to be refreshed by hand and committed. This is deliberately
##   not part of `docs`: rewriting it on every build left a dirty tree in each
##   worktree that had graphify data, and its first line is the commit the
##   graph was extracted at, so two branches that both built conflicted there
##   on every merge. Run this, read the diff, commit it on its own.
##
##   Needs graphify-out/graph.json  (`graphify update .`); without it the
##   build says so and leaves the committed file untouched.
docs-graph-refresh:
	@python3 $(REPO_ROOT)/web/build.py --refresh-graph

## docs-deps: fetch what the graph page needs, asking first  ·  used by `docs`
docs-deps:
	@set -e; \
	root='$(REPO_ROOT)'; auto='$(DOCS_AUTO)'; \
	ask() { \
	  case "$$auto" in \
	    1) return 0 ;; \
	    0) return 1 ;; \
	  esac; \
	  if [ ! -t 0 ]; then \
	    printf '  skipped: %s  (no terminal to ask; DOCS_AUTO=1 to allow)\n' "$$1"; \
	    return 1; \
	  fi; \
	  printf '  %s [y/N] ' "$$1"; \
	  read -r reply </dev/tty || return 1; \
	  case "$$reply" in y|Y|yes|YES) return 0 ;; *) printf '  skipped\n'; return 1 ;; esac; \
	}; \
	if [ ! -f "$$root/web/vendor/3d-force-graph.min.js" ] && \
	   ask "fetch the 3D graph renderer? 1.3 MB from unpkg, gitignored"; then \
	  mkdir -p "$$root/web/vendor"; \
	  if curl -sSLf -o "$$root/web/vendor/3d-force-graph.min.js" '$(GRAPH3D_URL)'; then \
	    printf '  vendored web/vendor/3d-force-graph.min.js\n'; \
	  else \
	    rm -f "$$root/web/vendor/3d-force-graph.min.js"; \
	    printf '  could not fetch the renderer — the flat graph still builds\n'; \
	  fi; \
	fi; \
	gf=''; \
	if command -v graphify >/dev/null 2>&1; then gf=graphify; \
	elif [ -x '$(GRAPHIFY_VENV)/bin/graphify' ]; then gf='$(GRAPHIFY_VENV)/bin/graphify'; \
	else \
	  py=''; \
	  for c in python3 python3.14 python3.13 python3.12 python3.11 python3.10; do \
	    if command -v "$$c" >/dev/null 2>&1 && \
	       "$$c" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3,10) else 1)' 2>/dev/null; then \
	      py="$$c"; break; \
	    fi; \
	  done; \
	  if [ -z "$$py" ]; then \
	    printf '  graphify needs Python 3.10+, none found — the flat graph still builds\n'; \
	  elif ask "install graphify? pip $(GRAPHIFY_PKG) into .venv-graphify, gitignored"; then \
	    if "$$py" -m venv '$(GRAPHIFY_VENV)' >/dev/null 2>&1 && \
	       '$(GRAPHIFY_VENV)/bin/python' -m pip install --quiet --disable-pip-version-check '$(GRAPHIFY_PKG)' >/dev/null 2>&1; then \
	      gf='$(GRAPHIFY_VENV)/bin/graphify'; \
	      printf '  installed graphify into .venv-graphify (%s)\n' "$$py"; \
	    else \
	      rm -rf '$(GRAPHIFY_VENV)'; \
	      printf '  graphify install failed — the flat graph still builds\n'; \
	      printf '    to see why: %s -m venv .venv-graphify && .venv-graphify/bin/pip install %s\n' "$$py" '$(GRAPHIFY_PKG)'; \
	    fi; \
	  fi; \
	fi; \
	if [ -n "$$gf" ]; then \
	  why=$$(python3 "$$root/web/graph/needs_refresh.py" "$$root" 2>/dev/null || echo ''); \
	  case "$$why" in \
	    absent) msg='extract the file-level graph? ~1 min, needed for the 3D page' ;; \
	    stale)  msg='sources changed since the graph was built — re-extract? ~1 min' ;; \
	    *)      msg='' ;; \
	  esac; \
	  if [ -n "$$msg" ] && ask "$$msg"; then \
	    ( cd "$$root" && "$$gf" update . ) || \
	      printf '  graphify update failed — building from whatever graph data exists\n'; \
	  fi; \
	fi; \
	exit 0

## docs-graph3d: vendor the 3D graph renderer
##   The one thing the 3D graph needs that is deliberately not in the tree:
##
##     web/vendor/3d-force-graph.min.js   1.3 MB of three.js and d3-force-3d.
##                                        A third-party bundle; gitignored.
##
##   Its data is committed as web/graph/files.json, so this is all a fresh
##   clone needs. The pages workflow runs this target for the same reason.
##   Without the renderer the build says so and falls back to the flat graph,
##   which is always correct and never a dead link.
##
##   `graphify update .` is still worth running before a refresh: it rewrites
##   graphify-out/graph.json, which is where both committed files come from.
GRAPH3D_URL ?= https://unpkg.com/3d-force-graph@1/dist/3d-force-graph.min.js
docs-graph3d:
	@mkdir -p $(REPO_ROOT)/web/vendor
	@curl -sSLf --retry 3 --retry-connrefused --retry-delay 2 \
	  -o $(REPO_ROOT)/web/vendor/3d-force-graph.min.js '$(GRAPH3D_URL)'
	@printf '  vendored web/vendor/3d-force-graph.min.js (%s KB)\n' \
	  "$$(( $$(wc -c <$(REPO_ROOT)/web/vendor/3d-force-graph.min.js) / 1024 ))"
	@printf '  `make docs` now builds the 3D graph from web/graph/files.json\n'

## docs-serve: build, then serve it on http://localhost:8080  ·  DOCS_PORT= to change
##   file:// cannot work: the fonts, the twin's WASM and the 3D renderer are
##   all fetched, so they need an origin. Ctrl-C to stop.
DOCS_PORT ?= 8080

docs-serve: docs
	@# Say who holds the port. http.server's own failure is a 25-line traceback
	@# ending in "Address already in use", which does not name the process and
	@# reads like the build broke.
	@if lsof -nP -iTCP:$(DOCS_PORT) -sTCP:LISTEN >/dev/null 2>&1; then \
	  printf '  port %s is already serving:\n' '$(DOCS_PORT)' >&2; \
	  lsof -nP -iTCP:$(DOCS_PORT) -sTCP:LISTEN \
	    | awk 'NR>1 {printf "    pid %s  %s\n", $$2, $$1}' >&2; \
	  printf '  stop it, or pick another: make docs-serve DOCS_PORT=8081\n' >&2; \
	  exit 1; \
	fi
	@printf '  serving web/dist on http://localhost:%s  (Ctrl-C to stop)\n' '$(DOCS_PORT)'
	@cd $(REPO_ROOT)/web/dist && python3 -m http.server $(DOCS_PORT)

## test-twin: build the WASM twin and replay its ranging scenarios
##   The page's firmware is the real modules/ultrawidelock_uwb through emcc, so this
##   is a firmware test that needs no board. Skipped loudly without emcc/node;
##   `make check` runs the same script as the `twin` suite.
test-twin:
	@bash $(REPO_ROOT)/tests/tooling/twin_suite.sh

## docs-clean: remove the built site
docs-clean:
	@rm -rf $(REPO_ROOT)/web/dist
	@printf '  removed web/dist\n'

## release-all: build every publishable image, then the site around them
##   The three release bundles plus a site whose flasher actually carries
##   firmware. Building and publishing a release stays entirely local: run it,
##   check the bundles under build/release/, push, then attach them with
##   `gh release create vN.N.N build/release/*/*.bin ...`.
##
##   The one exception is the website. .github/workflows/pages.yml renders and
##   deploys it, because publishing to Pages is the only step a laptop cannot
##   do. It is workflow_dispatch only -- nothing here happens on a push -- and
##   it fetches the firmware above from the latest release rather than building
##   any, so the site is only as current as the last release plus the last time
##   someone ran it.
##
##   Needs both toolchains and RELEASE_KEY=<path> for the signed Zephyr image.
##   Deliberately not the dev key `make dfu-key` writes.
release-all:
	@if [ -z '$(RELEASE_KEY)' ]; then \
	  printf '  release-all needs RELEASE_KEY=<path to the production key>\n' >&2; \
	  printf '  the checkout dev key must not sign a published image.\n' >&2; \
	  exit 1; \
	fi
	@$(MAKE) --no-print-directory esp-release
	@$(MAKE) --no-print-directory release RELEASE_KEY='$(RELEASE_KEY)'
	@$(MAKE) --no-print-directory nrf-release
	@$(MAKE) --no-print-directory docs-check
	@printf '\n  bundles in %s/release, site in web/dist\n' '$(ULTRAWIDELOCK_BUILD_ROOT)'
