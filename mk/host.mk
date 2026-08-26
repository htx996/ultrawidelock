# mk/host.mk — everything that runs on this machine: the host suites. No NCS
# toolchain, no ESP-IDF, no hardware. Output lands under build/host.

.PHONY: test sdk-check sdk-export test-san coverage cbmc check drift app-diff seam scope purity lint sca ci regress

##@ Test
## test: run the host test suite for our logic  (no NCS toolchain / hardware)
test:
	@$(REPO_ROOT)/tests/host/run.sh

## sdk-check: install the CMake package and build an external C consumer
sdk-check:
	@$(REPO_ROOT)/tests/sdk/run.sh

## sdk-export: the hardware-agnostic SDK as a versioned tarball  ->  build/sdk-export
sdk-export:
	@$(REPO_ROOT)/scripts/sdk-export.sh

## test-san: host suite rebuilt under ASan + UBSan  ·  memory-bug gate
test-san:
	@SAN=1 $(REPO_ROOT)/tests/host/run.sh

## coverage: line coverage of every host suite, 0% rows for the rest  ->  table + HTML
coverage:
	@$(REPO_ROOT)/tests/host/coverage.sh

## cbmc: bounded model-check the wire parsers  ·  memory-safety proof
cbmc:
	@$(REPO_ROOT)/tests/host/cbmc.sh

## check: every host-side suite under one banner  ->  live rows + summary table
##   A suite that passed over a working tree identical to this one is replayed
##   rather than re-run, and its row is marked `=` instead of `+`. The
##   fingerprint is the whole tree -- HEAD, the porcelain status and the content
##   of every dirty or untracked file -- so a skip means nothing changed, not
##   that a guess about the suite's inputs held. CHECK_NO_CACHE=1 runs the lot.
check:
	@$(REPO_ROOT)/scripts/test-runner.sh

## drift: constants and integration patch-state identity stay exact
drift:
	@$(REPO_ROOT)/tests/tooling/drift_suite.sh

## app-diff: how our copy of the door-lock app differs from pinned upstream
##   Reports, never fails: that application is ours to change. Read it before
##   raising PIN in scripts/bootstrap.sh, which is the one time the answer
##   decides something. Fetches the pinned path from GitHub. `make app-diff
##   ARGS=--stat` for the summary.
app-diff:
	@$(REPO_ROOT)/scripts/app-upstream-diff.sh $(ARGS)

## ci: every pull-request gate, in the order CI runs them  ·  run before opening one
##   .github/workflows/ci.yml runs this target rather than listing the gates
##   again, so what a pull request is judged by and what this prints cannot
##   drift apart. Cheapest first: a drifted constant fails in a second instead
##   of after two minutes of C suites.
##
##   The secret scan is the one difference. CI gives it its own job because it
##   needs the whole history; here it runs against your checkout when gitleaks
##   is installed, and says so loudly when it is not, because a skipped scan
##   that looks like a pass is worse than no scan.
ci:
	@$(MAKE) --no-print-directory drift
	@$(MAKE) --no-print-directory docs-check
	@if command -v gitleaks >/dev/null 2>&1; then \
	  gitleaks git --redact --no-banner "$(REPO_ROOT)"; \
	else \
	  printf '\n  !! gitleaks not installed — SECRET SCAN SKIPPED\n'; \
	  printf '     CI still runs it. install: brew install gitleaks\n\n'; \
	fi
	@CHECK_NO_CACHE=1 $(MAKE) --no-print-directory check

## regress: everything a machine can check without hardware  ·  run before a push
##   `make ci` is what a pull request is judged by, and it deliberately builds no
##   firmware. This is the superset a bench can run: the CI gates, then the one
##   suite CI cannot host (the integration patches, which need the network), then
##   every DWM3001CDK configuration and the size gate.
##
##   Needs the NCS toolchain, a bootstrapped ./workspace and the checkout's dev
##   signing key (`make bootstrap` and `make dfu-key`, once per clone).
##
##   The other two boards stay manual: `make nrf-build` and `make esp-build`.
##   With hardware on the bench, `make regress-hil` goes one tier further.
regress:
	@$(MAKE) --no-print-directory ci
	@SUITES="patchdrift" $(REPO_ROOT)/scripts/test-runner.sh
	@$(MAKE) --no-print-directory fw-regress

## seam: no call reaches the radio past the CCC STS seam
seam:
	@$(REPO_ROOT)/tests/tooling/uwb_seam_check.sh

## scope: no vendor radio API named outside the DW3000 engine file set
scope:
	@$(REPO_ROOT)/tests/tooling/uwb_engine_scope_check.sh

## purity: modules/ names no OS, each port tree names only its own
purity:
	@$(REPO_ROOT)/tests/tooling/port_purity_check.sh

## lint: cppcheck over the portable tree  ·  defects on paths no suite reaches
##   Skipped loudly, not failed, when cppcheck is missing: CI installs it.
##   modules/ and include/ only. ports/ and apps/ cannot be parsed without
##   Zephyr and ESP-IDF expanding their macros, so neither this nor `make sca`
##   reads them: that needs a compile database from a real target build.
lint:
	@$(REPO_ROOT)/tests/tooling/cppcheck_gate.sh

## sca: Clang Static Analyzer over the portable tree  ·  path-sensitive defects
##   The deeper pass `make lint` is not: it follows values across functions and
##   branches, so it reaches NULL-on-one-arm and use-after-free-on-the-error-path
##   bugs. Needs CodeChecker installed, so it is out of `make check` and CI:
##     make tools-install          (puts it in .venv-sca, where this gate looks)
##     CODECHECKER=<path> make sca (to point at an install somewhere else)
sca:
	@$(REPO_ROOT)/tests/tooling/codechecker_sca.sh
