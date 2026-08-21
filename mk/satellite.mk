# mk/satellite.mk — the second UWB anchor (docs/second-anchor.md).
#
# The other half of the inside/outside product: this board measures its own
# distance to the phone in the same ranging block the lock does, and returns it
# over the sealed Thread link. Lives in apps/nrf5340dk-satellite because it is a
# product front end, not a sample.
#
# One application, developed on the nRF5340 DK + DWM3000EVB, ported to a second
# DWM3001CDK later by changing SAT_BOARD (stage E). Reuses mk/cdk.mk's
# CDK_WEST / CDK_RUN / CDK_PROBE machinery the way mk/anchor.mk does, and for
# the same reason: two debug probes on one machine enumerate in a different
# order twenty minutes apart.

SAT_APP   := $(REPO_ROOT)/apps/nrf5340dk-satellite
SAT_BOARD ?= nrf5340dk/nrf5340/cpuapp

# Flattened board string, same as mk/anchor.mk: slashes would bury the build
# where the ELF paths stop being copy-pasteable.
SAT_BOARD_TAG := $(subst /,_,$(SAT_BOARD))
# Thread builds land beside the plain one, never on top of it: switching would
# otherwise reuse a CMake cache with the net-core image configured in or out,
# and fail in a way that reads as a code error.
SAT_SUFFIX := $(if $(filter 1,$(SAT_THREAD)),-thread,)
SAT_BUILD ?= $(ULTRAWIDELOCK_BUILD_ROOT)/satellite-$(SAT_BOARD_TAG)$(SAT_SUFFIX)

# Chip follows the board (`probe-rs attach` needs the right target or it fails
# in a way that reads like a dead board).
ifeq ($(SAT_BOARD),decawave_dwm3001cdk)
SAT_CHIP := nRF52833_xxAA
else
SAT_CHIP := nRF5340_xxAA_APPONLY
endif
override SAT_BUILD := $(abspath $(SAT_BUILD))

SAT_PRISTINE := $(if $(PRISTINE),always,auto)

# SAT_THREAD=1 adds the sealed link over Thread, and with it the nRF5340's
# net-core radio image. Opt-in rather than default: the Response arm margin
# measured WITHOUT OpenThread on this core is the baseline the Thread build has
# to be judged against, and there is no comparison to make if only one of the
# two can be built. It also keeps the UART-only first light available, which is
# what docs/second-anchor.md's risks section asks for.
#
# The conf files follow the board, and only for the radio. overlay-thread.conf
# is board-neutral; the nRF5340's second core is not, so that board layers
# overlay-thread-nrf5340.conf (the serialisation host) and sysbuild-thread.conf
# (the net-core image) on top. They go together: either half alone builds and
# then never sees a frame. The CDK takes the base file and nothing else --
# nRF52833 is single-core with the 802.15.4 radio on it, which is the same
# arrangement apps/dwm3001cdk-lock already runs standalone OpenThread on.
# Named relatively, like mk/cdk.mk's CDK_CONF: Zephyr resolves EXTRA_CONF_FILE
# against the application source directory, and the size report copies this
# string into a baseline that gets committed. An absolute path would put
# whoever's build tree it was into the repository.
ifeq ($(SAT_BOARD),decawave_dwm3001cdk)
SAT_THREAD_CONF := overlay-thread.conf
SAT_THREAD_SB   :=
else
SAT_THREAD_CONF := overlay-thread.conf;overlay-thread-nrf5340.conf
SAT_THREAD_SB   := -DSB_CONF_FILE=$(SAT_APP)/sysbuild-thread.conf
endif

ifeq ($(SAT_THREAD),1)
SAT_EXTRA_CONF := -DEXTRA_CONF_FILE="$(SAT_THREAD_CONF)" $(SAT_THREAD_SB)
else
SAT_EXTRA_CONF :=
endif

SAT_BAUD ?= 115200

# The DK pins its own probe the way the CDK does: by asking the silicon, not by
# enumeration order. Same script, retargeted -- the nRF5340's INFO.PART word
# lives at 0x00FF020C (read off this DK; the nRF52-era 0x10000100 reads back
# unrelated data here). The cache lives beside the CDK's, in the deny-all
# gitignored key dir, because a probe serial is machine-local state.
SAT_PROBE ?=
SAT_PROBE_CACHE ?= $(CDK_KEY_DIR)/sat-probe
SAT_PROBE_GOALS := sat-monitor nrf-monitor-rtt
ifeq ($(strip $(SAT_PROBE)),)
ifneq ($(filter $(SAT_PROBE_GOALS),$(MAKECMDGOALS)),)
SAT_PROBE := $(shell FIND_CHIP='$(SAT_CHIP)' FIND_FICR_ADDR=0x00FF020C \
  FIND_PART_PAT=5340 FIND_LABEL='nRF5340 DK' FIND_PART_NAME=nRF5340 \
  FIND_VAR_HINT=SAT_PROBE \
  '$(REPO_ROOT)/scripts/cdk-find-probe.sh' '$(SAT_PROBE_CACHE)')
endif
endif
SAT_PROBE_ARG := $(if $(SAT_PROBE),--probe '$(SAT_PROBE)')

# ---- size ---------------------------------------------------------------------
# The same three programs mk/cdk.mk runs on the lock, pointed at this app, and
# they matter here for the same reason: on the CDK the satellite is an nRF52833,
# 512 KB of flash and 128 KB of RAM, and the lock already runs at 85-92% of that
# part. Measurement and judgement stay separate: `sat-size` only ever reports,
# `sat-size-check` is the only one that can fail.
#
# ONE BASELINE PER VARIANT, named by board and Thread suffix. The four builds
# are four different images -- the DK has four times the RAM and a second core,
# and the Thread build carries a whole OpenThread stack -- so a single file
# would have one of them overwrite another's numbers and call the difference a
# regression.
SAT_SIZE_JSON     ?= $(SAT_BUILD)/size-report.json
SAT_SIZE_BASELINE ?= $(SAT_APP)/size-baseline-$(SAT_BOARD_TAG)$(SAT_SUFFIX).json
SAT_SIZE_REPORTS  ?= 1
SAT_SIZE_ARGS      = --build '$(SAT_BUILD)' --image satellite --json '$(SAT_SIZE_JSON)' \
                     $(if $(filter-out 0 n no off N NO OFF,$(SAT_SIZE_REPORTS)),--reports --run-prefix '$(CDK_WEST)')

.PHONY: sat-build sat-flash sat-monitor sat-term sat-size sat-size-check sat-size-baseline \
        nrf-monitor-rtt

##@ Satellite responder  ·  joins the phone's CCC round as responder 1 (stage B)
## sat-build: build the satellite  -> build/satellite-<board>
sat-build:
	@$(CDK_RUN) build -p $(SAT_PRISTINE) -b $(SAT_BOARD) \
	  -d $(SAT_BUILD) $(SAT_APP) $(if $(SAT_EXTRA_CONF),-- $(SAT_EXTRA_CONF))

## sat-size: what the satellite image costs and how much room is left  ·  measures only
sat-size:
	@test -f '$(SAT_BUILD)/satellite/zephyr/zephyr.elf' || { \
	  printf '  no ELF at %s/satellite/zephyr/zephyr.elf  ·  run `make sat-build` first\n' \
	    '$(SAT_BUILD)' >&2; exit 2; }
	@# From ./workspace like mk/cdk.mk's copy, so the west invocation the
	@# reports need resolves its manifest. Every path handed to the script is
	@# already absolute, so the working directory does not reach the output.
	@cd $(REPO_ROOT)/workspace 2>/dev/null || cd $(REPO_ROOT); \
	 python3 $(REPO_ROOT)/scripts/cdk-size.py $(SAT_SIZE_ARGS) \
	   $(if $(GITHUB_STEP_SUMMARY),--summary '$(GITHUB_STEP_SUMMARY)')
	@printf '  report  ·  %s\n\n' '$(SAT_SIZE_JSON)'

## sat-size-check: fail if the satellite image lost headroom against its baseline
sat-size-check:
	@test -f '$(SAT_SIZE_BASELINE)' || { \
	  printf '  no baseline at %s  ·  record one with `make sat-size-baseline` for this variant\n' \
	    '$(SAT_SIZE_BASELINE)' >&2; exit 2; }
	@# Suppressed for the measure step, as on the lock: the comparison below
	@# carries the same numbers with the baseline beside them, and two tables
	@# in one step summary is how a reader ends up reading the wrong one.
	@$(MAKE) --no-print-directory sat-size GITHUB_STEP_SUMMARY=
	@python3 $(REPO_ROOT)/scripts/cdk-size-compare.py \
	  --baseline '$(SAT_SIZE_BASELINE)' --current '$(SAT_SIZE_JSON)' \
	  $(if $(GITHUB_STEP_SUMMARY),--summary '$(GITHUB_STEP_SUMMARY)')

## sat-size-baseline: record this variant's current size as the one to compare against
sat-size-baseline: sat-size
	@python3 $(REPO_ROOT)/scripts/cdk-size-baseline.py \
	  --from '$(SAT_SIZE_JSON)' --out '$(SAT_SIZE_BASELINE)'

## sat-flash: flash the satellite built for SAT_BOARD
sat-flash:
	$(CDK_PROBE_GUARD)
	@$(CDK_RUN) flash -d $(SAT_BUILD) $(CDK_DEV_ID_ARG)

## sat-monitor: RTT console  ·  survives a shell thread that has died
##   Attaches on the DK's OWN pinned probe. It used CDK_PROBE_ARG until
##   2026-08-21 -- the other board's triple -- which with both probes attached
##   failed in the way that reads like a dead board.
sat-monitor:
	@probe-rs attach --chip $(SAT_CHIP) $(SAT_PROBE_ARG) \
	  $(SAT_BUILD)/satellite/zephyr/zephyr.elf

## nrf-monitor-rtt: RTT feed for the nRF5340 DK  ·  ctrl-c ends it
##   Takes the probe over (stops any capture already attached to this chip) and
##   appends a copy of everything to build/rtt-nrf5340dk.log (LOG=path moves it).
##   Picks the newest satellite ELF so the plain command matches whatever build
##   was flashed last, thread or not.
nrf-monitor-rtt:
	@command -v probe-rs >/dev/null 2>&1 || { printf '  probe-rs not found  ·  see `make tools`\n' >&2; exit 1; }
	-@pkill -f 'probe-rs attach --chip $(SAT_CHIP)' 2>/dev/null || true; sleep 1
	@log='$(if $(LOG),$(LOG),$(ULTRAWIDELOCK_BUILD_ROOT)/rtt-nrf5340dk.log)'; \
	elf=$$(/bin/ls -t $(ULTRAWIDELOCK_BUILD_ROOT)/satellite-*/satellite/zephyr/zephyr.elf 2>/dev/null | head -1); \
	[ -n "$$elf" ] || { printf '  no satellite ELF under %s  ·  make sat-build first\n' '$(ULTRAWIDELOCK_BUILD_ROOT)' >&2; exit 1; }; \
	printf '  RTT: nRF5340 DK  ·  elf %s  ·  copy -> %s  ·  ctrl-c ends\n' "$$elf" "$$log"; \
	exec script -aq "$$log" probe-rs attach --chip $(SAT_CHIP) $(SAT_PROBE_ARG) "$$elf"

## sat-term: serial console  ·  live logs + typeable shell (tio, 115200 8N1)
##   The Thread build runs a shell on BOTH the UART and RTT, so this and
##   sat-monitor are alternatives rather than rivals: use this to type, and RTT
##   when the question is why the board stopped answering this one.
sat-term:
	@command -v tio >/dev/null 2>&1 || { printf '  tio not found  ·  install: brew install tio\n' >&2; exit 1; }
	@port='$(PORT)'; \
	if [ -z "$$port" ]; then \
	  port=$$(ls /dev/cu.usbmodem* 2>/dev/null | tail -1); \
	fi; \
	if [ -z "$$port" ]; then \
	  printf '  no serial port found  ·  plug in the DK or pass PORT=/dev/cu.usbmodemXXXX\n' >&2; \
	  exit 1; \
	fi; \
	logargs=; [ -n '$(LOG)' ] && logargs='-L --log-file $(LOG)'; \
	printf '  tio %s  @ %s 8N1  ·  logs + shell (type help)  ·  ctrl-t q to quit\n' "$$port" '$(SAT_BAUD)'; \
	exec tio -b $(SAT_BAUD) $$logargs "$$port"
