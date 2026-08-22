# Makefile — single command entry point for the UltraWideLock firmware.
#
# A dispatcher, not an implementation: shared variables, the per-port includes
# below, and the `help` renderer. Every recipe lives in mk/<port>.mk, grouped by
# what it builds rather than by verb, so a target's neighbours are the other
# targets for the same board.
#
#   make                 # this grouped, colourised help
#   make build           # the DWM3001CDK lock (Matter over Thread) -> build/cdk-matter
#   make nrf-build       # the nRF5340 DK image                     -> build/nrf5340dk
#   make esp-build APP=matter-lock TARGET=esp32s3                   -> build/esp32-matter-lock-esp32s3
#   make test            # host suites (no toolchain, no hardware)
#
# Bare build/flash/monitor mean the DWM3001CDK because that is this project's
# headline target: one nRF52833 carrying the credential reader, the DW3110's ranging,
# a hand-written Matter node and an OpenThread MTD. The other two boards keep
# their own prefixes.

.DEFAULT_GOAL := help

# Must be computed before the includes: $(MAKEFILE_LIST) grows with each one,
# so `lastword` stops meaning "this file" the moment mk/*.mk is pulled in.
REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

# ---- one build root for the whole repo --------------------------------------
# Every producer — the three firmware ports, the host suites, the docs pipeline
# and the build signatures — derives its own subdirectory under this, so
# `make clean` is a single rm and nothing is left behind anywhere in the tree.
# Exported, because the shell scripts below read it rather than guessing.
#
#   build/cdk-matter/  cdk-reader/  cdk-selftest/
#   build/nrf5340dk/   nrf5340dk-blob/
#   build/esp32-<app>-<target>[-<variant>]/
#   build/host/        host_test* coverage/ cbmc/
#   build/_sig/        build signatures
#
# site/ is NOT here: it is publishable output, not an intermediate.
ULTRAWIDELOCK_BUILD_ROOT ?= $(REPO_ROOT)/build
export ULTRAWIDELOCK_BUILD_ROOT

# NCS version for both Zephyr ports. Matches scripts/nrf5340dk-build.sh.
NCS_VER ?= v3.3.0

# ---- the MCUboot image-signing key ------------------------------------------
# ONE key per checkout, shared by both Zephyr ports: it is the answer to "what
# firmware will a board of mine boot", and that question has one answer here.
# `make dfu-key` (mk/setup.mk) generates it and never overwrites it, both ports
# refuse to build without it (scripts/check-signing-key.sh), and it is
# gitignored, so a fresh clone or a new worktree fails at configure until it has
# one.
#
# Fresh checkouts keep it with the DWM3001CDK app. Existing checkouts retain the
# old ignored path below, so this migration cannot silently mint a second key.
#
# The path MUST be absolute; scripts/check-signing-key.sh explains why a
# relative one silently becomes MCUboot's published demo key. Exported so the
# port scripts read it rather than each keeping its own default.
LEGACY_CDK_KEY_DIR := $(REPO_ROOT)/firmware/keys
CDK_KEY_DIR        := $(REPO_ROOT)/apps/dwm3001cdk-lock/keys
SIGN_KEY ?= $(if $(wildcard $(LEGACY_CDK_KEY_DIR)/mcuboot_ec_p256.pem),$(LEGACY_CDK_KEY_DIR)/mcuboot_ec_p256.pem,$(CDK_KEY_DIR)/mcuboot_ec_p256.pem)
export SIGN_KEY

# ---- options forwarded to the firmware builds -------------------------------
# Set on the command line: make nrf-build PRETTY=1.
# The in-tree credential stack is the default; ULTRAWIDELOCK_SOURCE=0 selects the legacy
# Nordic archive for comparison or regression isolation.
CHIP     ?=
PRETTY   ?=
PRISTINE ?=
SELFTEST ?=
STRICT   ?=
# HA=1 opts into the Home Assistant variant. It must be set on BOTH bootstrap
# (applies the data-model patches) and build (layers ultrawidelock-ha.conf).
# Not hardware-validated.
HA            ?=
ULTRAWIDELOCK_SOURCE  ?=
ULTRAWIDELOCK_TRACE   ?=
NFC           ?=
CIR           ?=

# Serial monitor (make nrf-term / make esp-monitor). PORT auto-detects; override
# PORT/BAUD if the guess is wrong, and set LOG=file.log to tee the raw stream.
PORT     ?=
BAUD     ?= 115200
LOG      ?=


# ---- the ports, in the order `make help` should introduce them ---------------
include $(REPO_ROOT)/mk/cdk.mk
include $(REPO_ROOT)/mk/freertos-nrf52833.mk
include $(REPO_ROOT)/mk/nrf5340dk.mk
include $(REPO_ROOT)/mk/esp32.mk
# After the ports: it reuses cdk.mk's west launcher and probe guard, and it is a
# bench tool rather than a target anyone flashes a lock with.
include $(REPO_ROOT)/mk/anchor.mk
include $(REPO_ROOT)/mk/satellite.mk
include $(REPO_ROOT)/mk/witness.mk
include $(REPO_ROOT)/mk/setup.mk
include $(REPO_ROOT)/mk/host.mk
include $(REPO_ROOT)/mk/web.mk
include $(REPO_ROOT)/mk/extras.mk
