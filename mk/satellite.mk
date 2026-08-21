# mk/satellite.mk — the stage-B satellite responder (docs/second-anchor.md).
#
# One application, developed on the nRF5340 DK + DWM3000EVB, ported to a second
# DWM3001CDK later by changing SAT_BOARD (stage E). Reuses mk/cdk.mk's
# CDK_WEST / CDK_RUN / CDK_PROBE machinery the way mk/anchor.mk does, and for
# the same reason: two debug probes on one machine enumerate in a different
# order twenty minutes apart.

SAT_APP   := $(REPO_ROOT)/examples/zephyr/satellite
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
ifeq ($(SAT_THREAD),1)
SAT_EXTRA_CONF := -DEXTRA_CONF_FILE=$(SAT_APP)/overlay-thread.conf \
                  -DSB_CONF_FILE=$(SAT_APP)/sysbuild-thread.conf
else
SAT_EXTRA_CONF :=
endif

SAT_BAUD ?= 115200

.PHONY: sat-build sat-flash sat-monitor sat-term

##@ Satellite responder  ·  joins the phone's CCC round as responder 1 (stage B)
## sat-build: build the satellite  -> build/satellite-<board>
sat-build:
	@$(CDK_RUN) build -p $(SAT_PRISTINE) -b $(SAT_BOARD) \
	  -d $(SAT_BUILD) $(SAT_APP) $(if $(SAT_EXTRA_CONF),-- $(SAT_EXTRA_CONF))

## sat-flash: flash the satellite built for SAT_BOARD
sat-flash:
	$(CDK_PROBE_GUARD)
	@$(CDK_RUN) flash -d $(SAT_BUILD) $(CDK_DEV_ID_ARG)

## sat-monitor: RTT console  ·  survives a shell thread that has died
sat-monitor:
	$(CDK_PROBE_GUARD)
	@probe-rs attach --chip $(SAT_CHIP) $(CDK_PROBE_ARG) \
	  $(SAT_BUILD)/satellite/zephyr/zephyr.elf

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
