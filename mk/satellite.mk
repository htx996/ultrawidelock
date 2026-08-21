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
SAT_BUILD ?= $(ULTRAWIDELOCK_BUILD_ROOT)/satellite-$(SAT_BOARD_TAG)

# Chip follows the board (`probe-rs attach` needs the right target or it fails
# in a way that reads like a dead board).
ifeq ($(SAT_BOARD),decawave_dwm3001cdk)
SAT_CHIP := nRF52833_xxAA
else
SAT_CHIP := nRF5340_xxAA_APPONLY
endif
override SAT_BUILD := $(abspath $(SAT_BUILD))

SAT_PRISTINE := $(if $(PRISTINE),always,auto)

.PHONY: sat-build sat-flash sat-monitor

##@ Satellite responder  ·  joins the phone's CCC round as responder 1 (stage B)
## sat-build: build the satellite  -> build/satellite-<board>
sat-build:
	@$(CDK_RUN) build -p $(SAT_PRISTINE) -b $(SAT_BOARD) \
	  -d $(SAT_BUILD) $(SAT_APP)

## sat-flash: flash the satellite built for SAT_BOARD
sat-flash:
	$(CDK_PROBE_GUARD)
	@$(CDK_RUN) flash -d $(SAT_BUILD) $(CDK_DEV_ID_ARG)

## sat-monitor: RTT console  ·  the shell rides the DK's UART VCOM, not RTT
sat-monitor:
	$(CDK_PROBE_GUARD)
	@probe-rs attach --chip $(SAT_CHIP) $(CDK_PROBE_ARG) \
	  $(SAT_BUILD)/satellite/zephyr/zephyr.elf
