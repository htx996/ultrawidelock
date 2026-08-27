# mk/cdk.mk — the DWM3001CDK, this repo's primary target.
#
# Bare `build`, `flash`, `flash-erase` and `monitor` all mean this board and,
# unless you say otherwise, its Matter-over-Thread image. The standalone reader
# and the UWB self-test are the special cases and each keeps its own build
# directory, so no target can flash one image and then decode RTT against
# another's ELF.
#
# The app lives under apps/ rather than ports/: it is a product, not an OS
# backend.

# Sysbuild names each image after the application directory, so the per-image
# artifacts live under <build>/dwm3001cdk-lock. Named once here because three targets
# reach into it and a wrong guess fails as "no ELF", which reads like a dead board.
CDK_IMAGE := dwm3001cdk-lock
CDK_APP   := $(REPO_ROOT)/apps/dwm3001cdk-lock
CDK_BOARD := decawave_dwm3001cdk
CDK_CHIP  := nRF52833_xxAA

# WHICH DEBUG PROBE, and why this is not a preference. Empty means "let the tool
# pick", which is right with one probe attached and unsafe with two: probe
# enumeration order is NOT stable across replugs, so index 0 is a different board
# from one session to the next.
#
# MEASURED 2026-08-03. Two J-Links attached. `probe-rs list` returned them in one
# order at 15:53 and the reverse order at 16:13, with no cable touched in
# between. `make monitor` took index 0 both times and the second attempt died
# with "Target device responded with a FAULT response to the request" -- which
# reads like a dead board, not the wrong one. The flash targets would have
# written the other part and said nothing at all.
#
# Defaults to PROBE_RS_PROBE, so the variable probe-rs already documents covers
# the west targets too. Read the value out of `probe-rs list`; both forms work:
#   CDK_PROBE=VID:PID:Serial     CDK_PROBE=Serial
CDK_PROBE ?= $(PROBE_RS_PROBE)

# When neither is set and several probes are attached, identify the CDK by
# silicon instead of refusing: scripts/cdk-find-probe.sh reads FICR INFO.PART
# through each candidate (0x00052833 = nRF52833 = this board) and pins the
# winner's triple in the cache below, so the identification runs once per
# bench, not per flash. New caches live beside the app's ignored key; an old
# root-level cache remains readable so this move does not lose the probe choice.
# Delete the file to re-identify.
#
# Resolution happens at PARSE time, gated to the goals that touch a probe --
# it cannot happen inside a recipe, because this make expands every recipe
# line before the first one runs, so a cache written by line 1 is invisible
# to line 2 (measured on the macOS GNU make this repo is driven by).
CDK_PROBE_CACHE ?= $(if $(wildcard $(LEGACY_CDK_KEY_DIR)/cdk-probe),$(LEGACY_CDK_KEY_DIR)/cdk-probe,$(CDK_KEY_DIR)/cdk-probe)
CDK_PROBE_GOALS := flash flash-erase monitor monitor-rtt ota-window ota-recovery
ifeq ($(strip $(CDK_PROBE)),)
ifneq ($(filter $(CDK_PROBE_GOALS),$(MAKECMDGOALS)),)
CDK_PROBE := $(shell '$(REPO_ROOT)/scripts/cdk-find-probe.sh' '$(CDK_PROBE_CACHE)')
endif
endif
# probe-rs takes the whole triple; west's runners want the bare serial, which is
# the last colon-separated field of either form.
CDK_DEV_ID     := $(lastword $(subst :, ,$(CDK_PROBE)))
CDK_PROBE_ARG  := $(if $(CDK_PROBE),--probe '$(CDK_PROBE)')
CDK_DEV_ID_ARG := $(if $(CDK_DEV_ID),--dev-id $(CDK_DEV_ID))

# Refuse to guess where guessing writes flash. A wrong `monitor` prints an error
# and costs nothing; a wrong `flash` overwrites another board's part, so the
# ambiguity is fatal here rather than a warning nobody reads. Silent when one
# probe is attached, and silent when probe-rs is not installed -- this may not
# become a new reason a flash cannot run.
CDK_PROBE_GUARD = @if [ -z '$(CDK_PROBE)' ] && \
	    [ "$$(probe-rs list 2>/dev/null | grep -c '^\[')" -gt 1 ]; then \
	  printf '  more than one debug probe is attached and CDK_PROBE is unset.\n' >&2; \
	  probe-rs list >&2; \
	  printf '  Enumeration order is not stable, so this will not guess which board to write.\n' >&2; \
	  printf '  Pick one:  make $@ CDK_PROBE=<VID:PID:Serial>\n' >&2; \
	  printf '  Or once per shell:  export PROBE_RS_PROBE=<VID:PID:Serial>\n' >&2; \
	  exit 1; \
	fi

# ./workspace is a LINK into the machine's workspace store, made once per
# checkout by `make ws-link`. Every recipe below cds into it so west can find
# its manifest, so a checkout that was never linked fails at that cd -- and a
# bare `cd` failing says only
#
#   /bin/sh: line 0: cd: .../workspace: No such file or directory
#
# which names neither the cause nor the one-second fix. Every new worktree meets
# this exactly once, which is precisely when someone knows least about the
# store. The store itself already works; what was missing was saying so.
CDK_WS_GUARD = @if [ ! -d '$(REPO_ROOT)/workspace' ]; then \
	  printf '  this checkout has no ./workspace yet.\n' >&2; \
	  printf '  It is a link into the machine store, not a copy, and making it is instant\n' >&2; \
	  printf '  when the store already holds a tree for this branch:\n\n' >&2; \
	  printf '    make ws-link\n\n' >&2; \
	  printf '  `make ws-store` lists what this machine holds and who links to it.\n' >&2; \
	  printf '  Nothing there yet?  `make bootstrap` fetches it once, several GB.\n' >&2; \
	  exit 1; \
	fi

CDK_BUILD          ?= $(ULTRAWIDELOCK_BUILD_ROOT)/cdk-matter
CDK_READER_BUILD   ?= $(ULTRAWIDELOCK_BUILD_ROOT)/cdk-reader
CDK_SELFTEST_BUILD ?= $(ULTRAWIDELOCK_BUILD_ROOT)/cdk-selftest
CDK_CIRDIAG_BUILD  ?= $(ULTRAWIDELOCK_BUILD_ROOT)/cdk-cirdiag
CDK_MLGATE_BUILD   ?= $(ULTRAWIDELOCK_BUILD_ROOT)/cdk-mlgate
CDK_ANCHORLINK_BUILD ?= $(ULTRAWIDELOCK_BUILD_ROOT)/cdk-anchorlink$(if $(BENCH),-bench)
# Split out only so `monitor` can be pointed at an ELF without moving what the
# flash targets write. Same directory by default, which is the whole point.
CDK_RTT_BUILD      ?= $(CDK_BUILD)

# EVERY BUILD DIRECTORY IS MADE ABSOLUTE, and that is a bug fix rather than
# tidiness. west runs with the WORKSPACE as its working directory while every
# other consumer here resolves against the repo root, so a relative override
# like CDK_BUILD=build/foo built into workspace/build/foo and then failed at the
# setup-code step with "cannot read .../.config" -- a confusing error after a
# build that had actually succeeded, somewhere else. abspath resolves against
# make's cwd, the repo root, and is a no-op on an already-absolute path.
#
# `override` IS LOAD-BEARING. A command-line variable beats every plain
# assignment in a makefile, so without it these four lines are silently skipped
# for `make build CDK_BUILD=build/foo` -- the one spelling the fix exists for,
# and the one the help text tells you to use. It bit again on OTLOG=1. The
# environment form worked the whole time, which is what made it look fixed.
override CDK_BUILD          := $(abspath $(CDK_BUILD))
override CDK_READER_BUILD   := $(abspath $(CDK_READER_BUILD))
override CDK_SELFTEST_BUILD := $(abspath $(CDK_SELFTEST_BUILD))
override CDK_CIRDIAG_BUILD  := $(abspath $(CDK_CIRDIAG_BUILD))
override CDK_MLGATE_BUILD   := $(abspath $(CDK_MLGATE_BUILD))
override CDK_ANCHORLINK_BUILD := $(abspath $(CDK_ANCHORLINK_BUILD))
override CDK_RTT_BUILD      := $(abspath $(CDK_RTT_BUILD))

# PRISTINE=1 forces a from-scratch build. The recipes below hash their complete
# CMake argument tail and pass it again when an option changes, so supported
# switches reconfigure in place. It is still required when deliberately sharing
# one build directory between structurally different targets such as `reader`
# and `build`: each omits whole variable families that the other may have cached.
CDK_PRISTINE := $(if $(PRISTINE),always,auto)

# CIRDIAG_WINDOWS=1 re-arms the windowed-CIR dump in the `cirdiag` image. Off by
# default because with it armed this board never transmits a Response at all
# (measured, see the cirdiag target), and the taps it buys are worth 0.14 accuracy
# points to the classifier the capture feeds. Both states are explicit because
# CMake retains an omitted command-line Kconfig value in an existing cache.
CDK_CIRDIAG_WINDOWS := -DCONFIG_ULTRAWIDELOCK_CIRDIAG_CAPTURE_WINDOWS=$(if $(CIRDIAG_WINDOWS),y,n)

# RELEASE=1 appends the release overlay, which trades the 8 KB RTT ring for
# 7,168 B of RAM. Semicolon because EXTRA_CONF_FILE is a CMake list and later
# files win, so this can only ever override overlay-thread.conf. Changing the
# list changes the recipe's argument hash and reconfigures the existing build.
#
# LTO IS ON BY DEFAULT. `LTO=0` (also n/no/off) opts out, which is what you want
# when a stack trace has to name every frame.
#
# It was gated behind a walk-up unlock on hardware, because a size number cannot
# vouch for the ~1836 us ranging arm deadline under whole-program codegen. That
# gate passed 2026-08-02: two grants, a relock at 336 cm, a re-grant on the
# return leg at 45 cm, 604 RX with the STS live throughout.
#
# Applied to BOTH variants on purpose. Debug and release then differ only in the
# RTT ring size (8 KB vs 1 KB) and the log level (3 vs 1), their codegen is
# identical, and what you debug on the bench is what ships -- which matters most
# for exactly the timing bugs LTO could cause. RELEASE stays what it already
# claims to be: primarily a RAM lever (7,168 B from the RTT ring) plus a flash
# lever (20,568 B from errors-only logging), not a codegen one.
# Worth 41,084 B of flash. See apps/dwm3001cdk-lock/overlay-lto.conf.
#
# SMP=1 adds mcumgr over Bluetooth, which is what nRF Device Manager and nRF
# Connect for iOS speak. After errors-only logging and standalone OpenThread,
# debug+SMP leaves 12,764 B free, so RELEASE=1 is no longer required to fit.
# RELEASE=1 remains the shipping configuration. See apps/dwm3001cdk-lock/overlay-smp.conf
# for the measurements and the security note about the unpaired write endpoint.
# Ordered after overlay-release.conf so nothing it sets can be undone by it.
# OTLOG=1 turns OpenThread's own logging on, which is off by construction
# everywhere else: the level Kconfig depends on OPENTHREAD_DEBUG, so with debug
# off it falls through to 0 and the stack is silent no matter what
# CONFIG_LOG_DEFAULT_LEVEL says. Diagnosis only, never shipped, and it may not
# fit -- see apps/dwm3001cdk-lock/overlay-otlog.conf. Last in the list so nothing undoes it.
#
# CLIENT=1 adds the Matter client role: this lock opens ANOTHER Matter lock
# when the UWB gate fires. Not in any default, because it is the one option here
# that changes what the product IS rather than how it was built, and because it
# costs the OpenThread DNS client. A release image needs it explicitly:
#
#   make release RELEASE_KEY=<path> CLIENT=1
#
# Ordered after overlay-release.conf, like SMP, so nothing it sets can be
# undone. See apps/dwm3001cdk-lock/overlay-client.conf and docs/matter-binding.md.
#
# The LTO default is resolved HERE rather than assigned with `LTO ?= 1`, because
# make variables are global across the includes: `?=` would decide the other
# ports' default too, and mk/nrf5340dk.mk resolves the same LTO variable for
# itself. $(origin) distinguishes "user said nothing" from an explicit LTO=1, so
# each board owns its own default while sharing one spelling of the option. Both
# are on today, each behind its own walk-up on its own hardware.
LTO_SET  := $(filter-out undefined,$(origin LTO))
CDK_LTO  := $(filter-out 0 n no off N NO OFF,$(if $(LTO_SET),$(LTO),1))
# CLIENT=1 WITHOUT RELEASE=1 needs room made for it, and this is where.
# overlay-release.conf already buys that room by dropping the global log level;
# a debug image cannot take the same lever without silencing the client it was
# built to watch, so overlay-client-debug.conf trims by module instead. Applied
# automatically because the alternative is not a smaller image, it is a link
# that overflows FLASH by about 5,300 B -- a failure that reads as a broken tree
# rather than a budget. Ordered after overlay-client.conf so it is trimming a
# configuration that already exists.
CDK_CLIENT_DEBUG := $(if $(CLIENT),$(if $(RELEASE),,;overlay-client-debug.conf))
CDK_CONF := overlay-thread.conf$(if $(RELEASE),;overlay-release.conf)$(if $(SMP),;overlay-smp.conf)$(if $(CDK_LTO),;overlay-lto.conf)$(if $(CLIENT),;overlay-client.conf)$(CDK_CLIENT_DEBUG)$(if $(OTLOG),;overlay-otlog.conf)$(if $(ANCHOR),;overlay-anchor.conf)$(if $(SIDE),;overlay-side.conf)$(if $(LATCH),;overlay-latch.conf)

# The lock half of the two-anchor product (`make anchorlink`). Spelled out
# rather than layered onto CDK_CONF because two of these are not the caller's
# to choose: the WV3 consumer lives inside witness_link.c's WV2 one, so the
# latch overlay is mandatory, and the round has to advertise two responders or
# the satellite has no slot to answer in. BENCH=1 appends this desk's
# calibration LAST, where it can override the shipping defaults above it.
CDK_ANCHORLINK_CONF := overlay-thread.conf;overlay-latch.conf;overlays/bench-2resp.conf;overlay-anchorlink.conf$(if $(BENCH),;overlays/bench-anchorlink.conf)$(if $(CDK_LTO),;overlay-lto.conf)

# One-command real-board optimization lane. The Python driver owns the
# interactive lifecycle so Enter can end RTT capture and the local HTTP server
# without leaving a probe or background server behind. Its build is deliberately
# separate from the shipping image and always includes the bench-only latency,
# UWB, and SPI overlay.
INSTRUMENT_BUILD       ?= $(ULTRAWIDELOCK_BUILD_ROOT)/cdk-instrument
INSTRUMENT_OUTPUT      ?= $(REPO_ROOT)/internal/zephyr-opt/dashboard
INSTRUMENT_CAPTURE_DIR ?= $(REPO_ROOT)/internal/zephyr-opt/captures
INSTRUMENT_CONF        ?= overlay-thread.conf;overlay-lto.conf;$(REPO_ROOT)/tests/tooling/zephyr-opt-overlays/latency-uwb-spi.conf
INSTRUMENT_PROFILE     ?= thread+lto
INSTRUMENT_EXPERIMENT  ?= real-board
INSTRUMENT_RUN_ID      ?=
INSTRUMENT_ATTEMPTS    ?=
INSTRUMENT_WARMUP      ?= 0
INSTRUMENT_REJECTED    ?= 0
INSTRUMENT_TIMED_OUT   ?= 0
INSTRUMENT_PORT        ?= 8765
# Do not spell GNU Make's special recursive $(MAKE) variable in the instrument
# recipe. A line containing it executes even under `make -n`, which would turn a
# dry run into a real workflow. Override this only when the executable is not
# available as `make` on PATH.
INSTRUMENT_MAKE        ?= make

# ---- image signing -----------------------------------------------------------
# Which private key signs the image is the whole answer to "what will this lock
# boot", so it is never left to MCUboot's default -- that default is a key
# published in MCUboot's own repository. apps/dwm3001cdk-lock/sysbuild.cmake refuses to
# build with any of the seven.
#
# SIGN_KEY (top-level Makefile) is the checkout-wide default, shared with the
# nRF5340 DK, which signs with the same key for the same reason. CDK_KEY is kept
# as the per-build override because `make release` uses it to point one build at
# a production key without disturbing the checkout default.
#
# The path MUST be absolute. Sysbuild hands this symbol to the bootloader image
# through set_config_string(), never through a .conf file, so MCUboot's own
# base-directory search finds nothing and a relative path falls through to
# ${MCUBOOT_DIR}/<path> -- resolving INSIDE the MCUboot repo, which is how a
# wrong path turns silently back into the demo key.
#
# The inner quotes are part of the value: zephyr/cmake/modules/kconfig.cmake:264
# writes a command-line cache variable through verbatim, and a Kconfig string
# without quotes is a syntax error rather than a fallback.
#
# Applied to every CDK build variant. Changing it changes the recipe's argument
# hash, so an existing build directory is reconfigured with the new key.
CDK_KEY  ?= $(SIGN_KEY)
CDK_SIGN := -DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE='"$(CDK_KEY)"'

# ---- the version the BOARD reports -------------------------------------------
#
# Left unset, CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION defaults to "0.0.0+0" and
# every board ever built reports `v0.0.0.0` in its image list -- over the radio,
# over the cable, and on the flasher page. That is not a cosmetic gap. The only
# thing distinguishing two builds is then a SHA-256, which is correct and is
# what the update path matches on, but it means the one human-readable field in
# `make ota-smp-list` identifies nothing, and an operator comparing a board
# against a release is left comparing 16 hex digits by eye.
#
# Taken from the repository's VERSION file, so it moves when the project moves
# rather than when somebody remembers. A release can override it:
#
#     make release IMAGE_VERSION=0.3.1
#
# NOTE THAT THIS CHANGES THE IMAGE HASH, because the version sits in the MCUboot
# header and the SHA-256 TLV covers the header. That is correct and is the point
# -- two releases that differ only in their version really are different images,
# and a delta between them is a real delta. It does mean the deployed record has
# to be re-recorded after the first build that sets it, which `make flash` and
# the ota-* targets already do.
#
# The `+0` is imgtool's build number. Kept at zero: the build number is meant to
# distinguish rebuilds of one version, and nothing here would set it honestly.
IMAGE_VERSION ?= $(shell cat $(REPO_ROOT)/VERSION 2>/dev/null || echo 0.0.0)
CDK_SIGN += -DCONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION='"$(IMAGE_VERSION)+0"'

# ---- delta update over BLE ---------------------------------------------------
# modules/ultrawidelock_dfu has to reach BOTH images, and they need opposite halves of it:
# the patch APPLIER is compiled into MCUboot, because an application cannot
# rewrite the flash it is executing from, and the RECEIVER is compiled into the
# application, because the bootloader has no radio.
#
# So EXTRA_ZEPHYR_MODULES is set at the SYSBUILD level and not in
# apps/dwm3001cdk-lock/CMakeLists.txt, whose ZEPHYR_EXTRA_MODULES list is read only by the
# application image -- the bootloader would never see the module at all. What
# carries it across is that sysbuild copies every one of its own cache variables
# into each image's cache file (sysbuild_extensions.cmake:133-147), which is the
# same route NCS uses to get its MCUboot hooks compiled
# (nrf/modules/mcuboot/hooks/).
#
# The two CONFIG_ assignments are per-image and must not be swapped: each half
# is inert without a partition and a peer that the other image owns.
# ULTRAWIDELOCK_DFU_KEY is the IMAGE-signing key, deliberately. The application checks a
# staged update against its public half, so one secret authorises both what the
# bootloader will boot and what the radio will accept, and they cannot drift.
#
# THE RECEIVER IS OFF IN A DEBUG CLIENT IMAGE, and only there. That image does
# not otherwise fit: see overlay-client-debug.conf for the arithmetic and for
# what it already gave up in log strings before reaching for this.
#
# Off HERE and not in the overlay, because this is a command-line -D. Zephyr
# merges those last, through extra_kconfig_options.conf, so a `=n` in a .conf
# file would be silently overridden by the `=y` on this line and the image would
# still not fit -- with nothing in the log to say why.
#
# The module and the key stay on the line regardless. Dropping $(CDK_DFU)
# wholesale is the documented trap (see cirdiag below): it builds an image with
# no ultrawidelock_dfu module at all, which is a different image from the one
# being characterised.
#
# WHAT THIS DELIBERATELY DOES NOT DO: make the debug image ROOMIER than the
# release one. The partition map is untouched -- patch_staging stays reserved
# and unused -- so the debug build keeps a budget no larger than the release
# build's, and stays the tighter of the two. That is the property worth
# protecting: a debug image with headroom to spare is one that fills up with
# experiments and then discovers, at release time, that the shipping profile has
# not fitted for weeks. The one blind spot it creates is narrow and worth
# stating: growth INSIDE the DFU receiver itself is no longer visible from a
# debug build, only from a release one.
CDK_DFU_RX := $(if $(CLIENT),$(if $(RELEASE),y,n),y)
CDK_DFU  := -DEXTRA_ZEPHYR_MODULES='$(REPO_ROOT)/modules/ultrawidelock_dfu' \
            -DULTRAWIDELOCK_DFU_KEY='$(CDK_KEY)' \
            -Dmcuboot_CONFIG_ULTRAWIDELOCK_DFU_APPLIER=y \
            -DCONFIG_ULTRAWIDELOCK_DFU_RECEIVER=$(CDK_DFU_RX)

# DFU_LOG=1 makes the bootloader narrate what it does with a staged patch.
#
# Not on by default and not a size trim: MCUboot here has no LOG, no PRINTK and
# no RTT at all (apps/dwm3001cdk-lock/sysbuild/mcuboot.conf costs each one), and this turns
# three of them back on. Worth it exactly when the difference between "declined
# the patch" and "applied it and produced an image that fails validation" has to
# be visible, because from the outside those look identical -- both leave an
# erased staging partition and a board that does not run the new firmware.
#
# Read it with MCUboot's OWN elf, not the application's:
#   probe-rs attach --chip nRF52833_xxAA build/<dir>/mcuboot/zephyr/zephyr.elf
# The application re-initialises the RTT control block on every boot
# (CONFIG_SEGGER_RTT_INIT_MODE_ALWAYS in prj.conf, and it has to), so anything
# the bootloader printed is gone the moment the application starts.
CDK_DFU_LOG := -Dmcuboot_CONFIG_ULTRAWIDELOCK_DFU_APPLIER_LOG=$(if $(DFU_LOG),y,n) \
               -Dmcuboot_CONFIG_PRINTK=$(if $(DFU_LOG),y,n) \
               -Dmcuboot_CONFIG_USE_SEGGER_RTT=$(if $(DFU_LOG),y,n) \
               -Dmcuboot_CONFIG_RTT_CONSOLE=$(if $(DFU_LOG),y,n)

# ---- over-the-air update -----------------------------------------------------
#
# THE .hex, NOT THE .bin, AND THIS IS NOT A PREFERENCE. The build signs the
# application TWICE, in two separate imgtool runs, and ECDSA signatures are
# randomised -- so zephyr.signed.bin and zephyr.signed.hex carry the same code
# under different signatures, 64 bytes apart at the end. Only the .hex reaches
# merged.hex, so only the .hex is what a flashed board is actually running.
# An update is a DELTA against those exact bytes, so the wrong file produces a
# patch the board declines with "not for this image".
CDK_SIGNED_HEX := $(CDK_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.signed.hex
# The old serial-recovery path uploads a whole image and overwrites whatever was
# there, so it does not care which of the two it gets.
CDK_SIGNED := $(CDK_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.signed.bin
DFU_BAUD   ?= 115200

# WHAT THE BOARD IS RUNNING, recorded here because a delta cannot be computed
# without it and the board cannot be asked over the air. `flash` and a
# successful `dfu` both write this, so it tracks the board as long as nothing
# else programs it. If it goes stale the update is REFUSED rather than
# mis-applied -- the header carries a CRC of the from-image and the bootloader
# checks it -- so the failure mode is a wasted transfer, not a brick.
CDK_DEPLOYED ?= $(ULTRAWIDELOCK_BUILD_ROOT)/cdk-deployed/zephyr.signed.hex
CDK_PATCH    ?= $(CDK_BUILD)/update.wdfu

# The host tooling's Python dependencies, in a throwaway virtualenv rather than
# in the user's interpreter. detools creates the patch, cryptography signs its
# header, bleak carries it over Bluetooth.
CDK_OTA_VENV := $(ULTRAWIDELOCK_BUILD_ROOT)/ota-venv
CDK_OTA_PY   := $(CDK_OTA_VENV)/bin/python

# The ELF that goes with $(CDK_DEPLOYED), kept because `ota-window` needs the
# address of a symbol in the image the board is RUNNING, and the build tree only
# ever holds the image being built next. Those differ the moment you rebuild, and
# LTO renames the symbol, so a stale lookup writes a live value into the wrong
# RAM word and the push then fails with access denied for no visible reason.
CDK_DEPLOYED_ELF := $(dir $(CDK_DEPLOYED))zephyr.elf

# ULTRAWIDELOCK_TOOLCHAIN=env skips the nrfutil wrapper and runs west straight off PATH.
# scripts/nrf5340dk-build.sh carries the same escape hatch for the same reason:
# inside the NCS toolchain container CI uses, nrfutil's toolchain index is not
# reachable, so the wrapper cannot resolve a toolchain that is already there.
# firmware-builds.yml's dwm3001cdk job depends on this.
ifeq ($(ULTRAWIDELOCK_TOOLCHAIN),env)
CDK_WEST := west
else
CDK_WEST := nrfutil sdk-manager toolchain launch --ncs-version $(NCS_VER) -- west
endif

# Every recipe runs from ./workspace so west finds its manifest.
CDK_RUN = cd $(REPO_ROOT)/workspace && $(CDK_WEST)

# ---- size ---------------------------------------------------------------------
# RAM is the scarcest thing on this board and the easiest to spend by accident:
# the shipping Matter image runs above 92% of the nRF52833's 128 KB, so a new static buffer
# is a decision rather than a detail. These two targets make that visible and
# then enforce it. Measurement and judgement are deliberately separate programs:
# `cdk-size` only ever reports, `cdk-size-check` is the only one that can fail.
#
# CDK_SIZE_REPORTS=0 skips Zephyr's ram_report/rom_report. They are the numbers
# the RAM work in this repo already quotes, so they are on by default, but they
# cost ~73 s (measured, this image), they need the toolchain, and they are a
# ninja target with real dependencies -- so on a STALE tree they relink rather
# than merely measure. Everything else comes straight out of the ELF and needs
# no toolchain at all, which is why a report is still possible without them.
CDK_SIZE_JSON     ?= $(CDK_BUILD)/size-report.json
CDK_SIZE_BASELINE ?= $(REPO_ROOT)/apps/dwm3001cdk-lock/size-baseline.json
CDK_SIZE_REPORTS  ?= 1
CDK_SIZE_ARGS      = --build '$(CDK_BUILD)' --image $(CDK_IMAGE) --json '$(CDK_SIZE_JSON)' \
                     $(if $(filter-out 0 n no off N NO OFF,$(CDK_SIZE_REPORTS)),--reports --run-prefix '$(CDK_WEST)')

.PHONY: build rebuild instrument reader selftest cirdiag mlgate anchorlink flash flash-erase monitor monitor-rtt dfu release \
        cdk-size cdk-size-check cdk-size-baseline \
        dfu-serial fota fota-build fota-done fota-confirm ota-patch ota-push ota-smp ota-smp-push ota-smp-list ota-window ota-recovery ota-deps ota-fan ota-local \
        cdk-ultrawidelock-matter-thread cdk-reader cdk-flash cdk-flash-erase cdk-rtt

# ---- the CMake argument tail ---------------------------------------------------
# WEST RE-RUNS CMAKE WHENEVER ANYTHING FOLLOWS `--`, and it does not care that
# the build directory is already configured: `west build` sets run_cmake from a
# non-empty cmake_opts alone, in the branch it takes for an existing Zephyr build
# (workspace/zephyr/scripts/west_commands/build.py). These recipes always passed a
# tail, so every one of these six targets reconfigured sysbuild and both of its
# images on every invocation, whether or not anything had changed.
#
# MEASURED 2026-08-23, same tree and same warm caches, tail forced against tail
# skipped: a `make build` with nothing changed went 50.2 s -> 2.8 s, and one that
# rebuilds a single app source file went 53.0 s -> 21.5 s. The ~47 s is three
# CMake configures -- sysbuild, mcuboot and the lock image -- each regenerating
# what it had already written. PRISTINE=1 is unaffected and is meant to be: it
# passes the tail unconditionally, because the directory the cache lives in is
# about to be deleted.
#
# So the tail is passed only when it can matter: no configured build directory,
# PRISTINE=1, or the arguments changed since the last successful build. CMake
# keeps them in CMakeCache.txt, and ninja re-runs CMake on its own when a Kconfig
# fragment, an overlay or a CMakeLists file changes -- that is what
# CMAKE_CONFIGURE_DEPENDS is for. VERIFIED by touching overlay-thread.conf with no
# tail: exactly one reconfigure, and a zephyr/.config byte-identical to the one
# the always-reconfigure recipe writes.
#
# A toggleable command-line Kconfig value must encode BOTH states. If its `=n`
# form disappears when switched off, CMake keeps the earlier cached `=y` even
# though this mechanism correctly notices the changed tail and reconfigures.
#
# The stamp holds a checksum rather than the arguments themselves so the
# comparison stays a single make word -- the real value carries a
# semicolon-separated list, quotes and absolute paths, none of which survive
# make's word splitting intact. cksum because it is POSIX and is therefore also
# in the NCS container CI builds in.
#
# $(call cdk_tail,<args variable>,<build-dir variable>) yields the tail or
# nothing, and $(call cdk_stamp,...) records what was passed. Both take the NAMES
# of the two variables, never their values: $(call) splits its arguments on
# commas, and every one of these argument lists carries absolute paths.
#
# Recursive `=` throughout, so the two sub-shells run only while the one recipe
# being built expands -- `:=` would run them for all six on every `make` in this
# repo, `make help` included. $(strip) on the freshness test is load-bearing: a
# line continuation expands to a space, and a lone space is a true condition for
# $(if), which would skip the tail forever.
cdk_args_id  = $(firstword $(shell printf '%s' '$($(1))' | cksum))
cdk_stamp_at = $($(2))/.ultrawidelock-cmake-args
cdk_fresh    = $(strip $(if $(PRISTINE),,$(if $(wildcard $($(2))/CMakeCache.txt),\
                 $(filter $(call cdk_args_id,$(1)),\
                   $(firstword $(shell cat '$(call cdk_stamp_at,$(1),$(2))' 2>/dev/null))))))
cdk_tail     = $(if $(call cdk_fresh,$(1),$(2)),,-- $($(1)))
cdk_stamp    = printf '%s\n' '$(call cdk_args_id,$(1))' > '$(call cdk_stamp_at,$(1),$(2))'

# A configured build directory records absolute paths, and CMAKE_CACHEFILE_DIR is
# the one that names the directory the cache was written FOR. Move the checkout --
# renaming a home directory does it -- and every path in there is wrong: `-p auto`
# correctly notices the changed app dir, then resolves pristine.cmake through the
# stale ZEPHYR_BASE and exits 1. West cannot recover the directory on its own, so
# `make build` stays wedged until someone deletes it by hand. Delete it here
# instead, and say so: the cost is one rebuild, and the alternative is a failure
# whose message names a path that does not exist.
cdk_scrub = if [ -f '$($(1))/CMakeCache.txt' ] && \
              ! grep -qxF 'CMAKE_CACHEFILE_DIR:INTERNAL=$($(1))' '$($(1))/CMakeCache.txt'; then \
              printf '  build dir was configured for another path  ·  removing %s\n' '$($(1))'; \
              rm -rf '$($(1))'; \
            fi

##@ DWM3001CDK  ·  the lock (bare targets mean this board)
## build: the DWM3001CDK lock, reader + Matter over Thread  -> build/cdk-matter
CDK_BUILD_ARGS = -DEXTRA_CONF_FILE="$(CDK_CONF)" -DCONFIG_ULTRAWIDELOCK_MATTER_BLE=y \
                 $(CDK_SIGN) $(CDK_DFU) $(CDK_DFU_LOG)
build:
	$(CDK_WS_GUARD)
	@$(call cdk_scrub,CDK_BUILD)
	@$(CDK_RUN) build -p $(CDK_PRISTINE) -b $(CDK_BOARD) \
	  -d $(CDK_BUILD) $(CDK_APP) \
	  $(call cdk_tail,CDK_BUILD_ARGS,CDK_BUILD)
	@$(call cdk_stamp,CDK_BUILD_ARGS,CDK_BUILD)
	@python3 $(REPO_ROOT)/scripts/spake2p_verifier.py \
	  --from-config $(CDK_BUILD)/$(CDK_IMAGE)/zephyr/.config

## instrument: build, flash, capture, render, serve, and open the DWM3001CDK dashboard
#   Interactive bench workflow. Press Enter once to finish RTT capture and once
#   more to stop the localhost server. It never erases board state.
instrument:
	@python3 '$(REPO_ROOT)/tests/tooling/zephyr_opt_instrument.py' \
	  --repo-root '$(REPO_ROOT)' \
	  --make '$(INSTRUMENT_MAKE)' \
	  --build '$(INSTRUMENT_BUILD)' \
	  --output '$(INSTRUMENT_OUTPUT)' \
	  --capture-dir '$(INSTRUMENT_CAPTURE_DIR)' \
	  --sign-key '$(SIGN_KEY)' \
	  --conf '$(INSTRUMENT_CONF)' \
	  --profile '$(INSTRUMENT_PROFILE)' \
	  --experiment '$(INSTRUMENT_EXPERIMENT)' \
	  --port '$(INSTRUMENT_PORT)' \
	  $(if $(strip $(INSTRUMENT_RUN_ID)),--run-id '$(INSTRUMENT_RUN_ID)') \
	  $(if $(strip $(INSTRUMENT_ATTEMPTS)),--attempts '$(INSTRUMENT_ATTEMPTS)') \
	  --warmup '$(INSTRUMENT_WARMUP)' \
	  --rejected '$(INSTRUMENT_REJECTED)' \
	  --timed-out '$(INSTRUMENT_TIMED_OUT)'

## rebuild: force a clean pristine build of the Matter image
rebuild:
	@$(MAKE) --no-print-directory build PRISTINE=1

## reader: the same board WITHOUT Matter        -> build/cdk-reader
CDK_READER_ARGS = $(CDK_SIGN)
reader:
	@$(call cdk_scrub,CDK_READER_BUILD)
	@$(CDK_RUN) build -p $(CDK_PRISTINE) -b $(CDK_BOARD) \
	  -d $(CDK_READER_BUILD) $(CDK_APP) \
	  $(call cdk_tail,CDK_READER_ARGS,CDK_READER_BUILD)
	@$(call cdk_stamp,CDK_READER_ARGS,CDK_READER_BUILD)

## selftest: one-shot UWB init self-test at boot  -> build/cdk-selftest
CDK_SELFTEST_ARGS = -DEXTRA_CONF_FILE=overlays/uwb-selftest.conf $(CDK_SIGN)
selftest:
	@$(call cdk_scrub,CDK_SELFTEST_BUILD)
	@$(CDK_RUN) build -p $(CDK_PRISTINE) -b $(CDK_BOARD) \
	  -d $(CDK_SELFTEST_BUILD) $(CDK_APP) \
	  $(call cdk_tail,CDK_SELFTEST_ARGS,CDK_SELFTEST_BUILD)
	@$(call cdk_stamp,CDK_SELFTEST_ARGS,CDK_SELFTEST_BUILD)

## cirdiag: the Matter image plus an unattended CIR capture cycle  -> build/cdk-cirdiag
#   Flash it with `flash`, NEVER `flash-erase`: the erase takes the commissioned
#   credential the capture walk-up needs, and costs a re-commissioning.
#   Every flag `build` passes is repeated here, deliberately and not by include:
#   dropping $(CDK_DFU) alone silently built an image with no ultrawidelock_dfu module at
#   all, which is a different image from the one being characterised and cannot
#   be updated over Bluetooth. If `build` gains a flag, this needs it too.
CDK_CIRDIAG_ARGS = -DEXTRA_CONF_FILE="$(CDK_CONF);overlays/cirdiag.conf" \
                   -DCONFIG_ULTRAWIDELOCK_MATTER_BLE=y $(CDK_CIRDIAG_WINDOWS) \
                   $(CDK_SIGN) $(CDK_DFU) $(CDK_DFU_LOG)
cirdiag:
	@$(call cdk_scrub,CDK_CIRDIAG_BUILD)
	@$(CDK_RUN) build -p $(CDK_PRISTINE) -b $(CDK_BOARD) \
	  -d $(CDK_CIRDIAG_BUILD) $(CDK_APP) \
	  $(call cdk_tail,CDK_CIRDIAG_ARGS,CDK_CIRDIAG_BUILD)
	@$(call cdk_stamp,CDK_CIRDIAG_ARGS,CDK_CIRDIAG_BUILD)
	@python3 $(REPO_ROOT)/scripts/spake2p_verifier.py \
	  --from-config $(CDK_CIRDIAG_BUILD)/$(CDK_IMAGE)/zephyr/.config

## mlgate: DWM3001CDK image that runs the LOS/NLOS classifier in the unlock path
#   Same repetition rule as `cirdiag`: every flag `build` passes is repeated
#   here on purpose, because dropping one silently characterises a different
#   image.
CDK_MLGATE_ARGS = -DEXTRA_CONF_FILE="$(CDK_CONF);overlays/mlgate.conf" \
                  -DCONFIG_ULTRAWIDELOCK_MATTER_BLE=y \
                  $(CDK_SIGN) $(CDK_DFU) $(CDK_DFU_LOG)
mlgate:
	@$(call cdk_scrub,CDK_MLGATE_BUILD)
	@$(CDK_RUN) build -p $(CDK_PRISTINE) -b $(CDK_BOARD) \
	  -d $(CDK_MLGATE_BUILD) $(CDK_APP) \
	  $(call cdk_tail,CDK_MLGATE_ARGS,CDK_MLGATE_BUILD)
	@$(call cdk_stamp,CDK_MLGATE_ARGS,CDK_MLGATE_BUILD)
	@python3 $(REPO_ROOT)/scripts/spake2p_verifier.py \
	  --from-config $(CDK_MLGATE_BUILD)/$(CDK_IMAGE)/zephyr/.config

## anchorlink: the lock half of the two-anchor inside/outside pair  ·  BENCH=1 for the desk
#   The other half is `make sat-build SAT_THREAD=1` (apps/satellite).
#   Flash this with `make flash CDK_BUILD=build/cdk-anchorlink` and NEVER
#   flash-erase: the anchor key enrolled from the reader image lives in the
#   settings partition a full erase takes with it.
CDK_ANCHORLINK_ARGS = -DEXTRA_CONF_FILE="$(CDK_ANCHORLINK_CONF)" \
                      -DCONFIG_ULTRAWIDELOCK_MATTER_BLE=y \
                      $(CDK_SIGN) $(CDK_DFU) $(CDK_DFU_LOG)
anchorlink:
	@$(call cdk_scrub,CDK_ANCHORLINK_BUILD)
	@$(CDK_RUN) build -p $(CDK_PRISTINE) -b $(CDK_BOARD) \
	  -d $(CDK_ANCHORLINK_BUILD) $(CDK_APP) \
	  $(call cdk_tail,CDK_ANCHORLINK_ARGS,CDK_ANCHORLINK_BUILD)
	@$(call cdk_stamp,CDK_ANCHORLINK_ARGS,CDK_ANCHORLINK_BUILD)
	@python3 $(REPO_ROOT)/scripts/spake2p_verifier.py \
	  --from-config $(CDK_ANCHORLINK_BUILD)/$(CDK_IMAGE)/zephyr/.config


## flash: flash the DWM3001CDK over its on-board J-Link OB
flash:
	$(CDK_WS_GUARD)
	$(CDK_PROBE_GUARD)
	@# `flash` does not rebuild, and a stale hex flashes without a word. On
	@# 2026-08-07 that wrote a 00:35-era image at 21:05 as "the fix committed
	@# at 21:04" -- the board then reproduced the fixed bug for two hours and
	@# every log read as the fix having failed. Say the ages out loud; the
	@# flash still runs, because flashing an old build on purpose is
	@# legitimate and this cannot tell intent from accident.
	@if [ -f '$(CDK_SIGNED_HEX)' ]; then \
	  hex_t=$$(stat -f %m '$(CDK_SIGNED_HEX)' 2>/dev/null || stat -c %Y '$(CDK_SIGNED_HEX)'); \
	  head_t=$$(git -C '$(REPO_ROOT)' log -1 --format=%ct 2>/dev/null || echo 0); \
	  if [ "$$hex_t" -lt "$$head_t" ]; then \
	    printf '\n  *** the image being flashed is OLDER than your last commit ***\n'; \
	    printf '      built:  %s\n' "$$(date -r $$hex_t '+%F %T' 2>/dev/null || date -d @$$hex_t '+%F %T')"; \
	    printf '      HEAD:   %s\n' "$$(date -r $$head_t '+%F %T' 2>/dev/null || date -d @$$head_t '+%F %T')"; \
	    printf '      run `make build` first if you meant to flash what you just committed.\n\n'; \
	  fi; \
	fi
	@$(CDK_RUN) flash $(CDK_DEV_ID_ARG) -d $(CDK_BUILD)
	@# Record what the board now runs, so `make dfu` can diff against it. A
	@# delta needs the exact bytes that are on the part, and once the probe is
	@# gone there is no way to ask.
	@if [ -f '$(CDK_SIGNED_HEX)' ]; then \
	  mkdir -p '$(dir $(CDK_DEPLOYED))' && cp '$(CDK_SIGNED_HEX)' '$(CDK_DEPLOYED)'; \
	  cp '$(CDK_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.elf' '$(CDK_DEPLOYED_ELF)' 2>/dev/null || true; \
	fi

## flash-erase: full chip erase + flash the DWM3001CDK
flash-erase:
	$(CDK_WS_GUARD)
	$(CDK_PROBE_GUARD)
	@$(CDK_RUN) flash --erase $(CDK_DEV_ID_ARG) -d $(CDK_BUILD)

## dfu: update the board over Bluetooth  ·  no cable, no probe
dfu:
	@$(MAKE) --no-print-directory build
	@$(MAKE) --no-print-directory ota-patch
	@$(MAKE) --no-print-directory ota-push

## fota: make the file an iPhone can install, and say how  ·  needs SMP=1
FOTA_VERSION   ?= 1.0.0
CDK_FOTA_BUILD ?= $(ULTRAWIDELOCK_BUILD_ROOT)/cdk-smp-img
CDK_FOTA_BUILD := $(abspath $(CDK_FOTA_BUILD))
CDK_FOTA       := $(CDK_BUILD)/ultrawidelock-fota.bin

fota:
	@$(MAKE) --no-print-directory fota-build \
	  SMP=1 RELEASE=1 CDK_BUILD='$(CDK_FOTA_BUILD)'

fota-build:
	@$(MAKE) --no-print-directory build
	@$(MAKE) --no-print-directory ota-patch
	@$(CDK_OTA_PY) $(REPO_ROOT)/scripts/ultrawidelock_patch.py wrap '$(CDK_PATCH)' \
	  --version '$(FOTA_VERSION)' --out-dir '$(CDK_BUILD)' \
	  --from-image '$(CDK_DEPLOYED)' --to-image '$(CDK_SIGNED_HEX)'
	@printf '\n  ---- put this on the phone ----------------------------------\n\n'
	@ls -t '$(CDK_BUILD)'/ultrawidelock-*.zip '$(CDK_BUILD)'/ultrawidelock-*.bin 2>/dev/null | head -2 | sed 's/^/  /'
	@printf '\n  The name carries both hashes: ultrawidelock-<applies to>-to-<produces>.\n'
	@printf '  `make ota-smp-list` prints what the board is running -- the first\n'
	@printf '  half of the name must match it, or the board will refuse the patch.\n'
	@printf '  DELETE OLD COPIES ON THE PHONE. A stale file that still looks\n'
	@printf '  plausible is the one failure this whole path cannot catch for you.\n\n'
	@printf '  1. AirDrop either file to the phone, or drop it in Files\n'
	@printf '  2. Press SW2 on the board  (or Apple Home -> Turn On Pairing Mode)\n'
	@printf '  3. nRF Device Manager -> connect to "ultrawidelock"\n'
	@printf '  4. Images tab -> SELECT FILE -> that file -> UPLOAD\n'
	@printf '  5. Device tab -> Reset,  then wait ~30 s\n\n'
	@printf '  Use the Images tab, NOT the guided firmware-upgrade wizard: that\n'
	@printf '  flow waits for a second image to confirm and a reconnect that the\n'
	@printf '  bootloader apply outlasts.\n\n'
	@printf '  6. back here, run  make fota-done\n\n'
	@printf '  Step 6 is not optional. A delta is computed against the exact bytes\n'
	@printf '  on the board, and only this machine keeps the record of what those\n'
	@printf '  are -- a push from the phone is invisible to it. Skip step 6 and the\n'
	@printf '  NEXT update is built from the wrong base and the board refuses it.\n\n'
	@printf '  The window closes after five minutes. Reset is refused outside it\n'
	@printf '  unless a patch is already staged, which is deliberate -- otherwise\n'
	@printf '  anyone in radio range could reboot the lock in a loop.\n\n'

## fota-done: after a phone push, confirm it landed and record what the board runs
fota-done:
	@$(MAKE) --no-print-directory fota-confirm \
	  SMP=1 RELEASE=1 CDK_BUILD='$(CDK_FOTA_BUILD)'

fota-confirm: $(CDK_OTA_PY)
	@if [ -n '$(OTA_SERIAL)' ]; then \
	  $(CDK_PORT_RESOLVE); \
	  $(CDK_OTA_PY) $(REPO_ROOT)/scripts/ultrawidelock_smp.py \
	    --expect '$(CDK_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.signed.bin' \
	    --serial "$$port" || exit 1; \
	else \
	  $(CDK_OTA_PY) $(REPO_ROOT)/scripts/ultrawidelock_smp.py \
	    --expect '$(CDK_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.signed.bin' \
	    $(if $(OTA_NAME),--name '$(OTA_NAME)') || exit 1; \
	fi
	@mkdir -p '$(dir $(CDK_DEPLOYED))'
	@cp '$(CDK_SIGNED_HEX)' '$(CDK_DEPLOYED)'
	@cp '$(CDK_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.elf' '$(CDK_DEPLOYED_ELF)'
	@printf '  recorded as deployed  ·  %s\n' '$(CDK_DEPLOYED)'

## ota-patch: build a signed delta from the deployed image to the built one
ota-patch: $(CDK_OTA_PY)
	@if [ ! -f '$(CDK_DEPLOYED)' ]; then \
	  printf '  no record of what the board is running  ·  %s\n' '$(CDK_DEPLOYED)' >&2; \
	  printf '  A delta needs the image it starts from. Either `make flash` once over SWD\n' >&2; \
	  printf '  to set the record, or point CDK_DEPLOYED at the signed .hex it is running.\n' >&2; \
	  exit 1; \
	fi
	@$(CDK_OTA_PY) $(REPO_ROOT)/scripts/ultrawidelock_patch.py build \
	  --from '$(CDK_DEPLOYED)' --to '$(CDK_SIGNED_HEX)' \
	  --build-dir '$(CDK_BUILD)' --key '$(CDK_KEY)' --out '$(CDK_PATCH)'

## ota-push: send an already-built patch over Bluetooth
ota-push: $(CDK_OTA_PY)
	@test -f '$(CDK_PATCH)' || { printf '  no patch at %s  ·  run `make ota-patch`\n' '$(CDK_PATCH)' >&2; exit 1; }
	@$(CDK_OTA_PY) $(REPO_ROOT)/scripts/ultrawidelock_push.py '$(CDK_PATCH)' \
	  $(if $(OTA_NAME),--name '$(OTA_NAME)')
	@mkdir -p '$(dir $(CDK_DEPLOYED))' && cp '$(CDK_SIGNED_HEX)' '$(CDK_DEPLOYED)'
	@cp '$(CDK_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.elf' '$(CDK_DEPLOYED_ELF)' 2>/dev/null || true
	@printf '  recorded as deployed  ·  %s\n' '$(CDK_DEPLOYED)'

## ota-smp: push the patch over mcumgr instead, exactly as a phone would
##   OTA_SERIAL=auto   go down the cable instead of the radio (or give a port)
#
# THE CABLE IS A VARIABLE, NOT A SECOND PAIR OF TARGETS, and that is the point
# rather than a shortcut. uart0 carries the same mcumgr conversation the radio
# carries -- same handler, same signature check, same update window -- so a
# target that behaved differently would be claiming a difference that does not
# exist. What changes is one argument to one script.
#
# `auto` picks the first /dev/cu.usbmodem*, which on a machine with one board
# attached is the J-Link OB's VCOM. Give an explicit port when more than one
# probe is plugged in; `ls /dev/cu.usbmodem*` lists them.
#
# SETS ITS OWN CONFIGURATION, like `fota` and for the same reason: this target
# is definitionally the mcumgr path, and a board without SMP does not speak it
# at all. Inheriting a bare `make`'s defaults would look in build/cdk-matter --
# a different, non-SMP image -- and either find no patch or push one built from
# the wrong base. RELEASE goes with it because SMP does not fit without it.
ota-smp:
	@$(MAKE) --no-print-directory ota-smp-push \
	  SMP=1 RELEASE=1 CDK_BUILD='$(CDK_FOTA_BUILD)'

# Resolves OTA_SERIAL into $$port, or exits saying so. `auto` and `1` both mean
# "find it", so `make ota-smp OTA_SERIAL=1` does the obvious thing.
CDK_PORT_RESOLVE = port='$(OTA_SERIAL)'; \
	if [ "$$port" = auto ] || [ "$$port" = 1 ]; then \
	  port=$$(ls /dev/cu.usbmodem* 2>/dev/null | head -n1); \
	fi; \
	if [ -z "$$port" ]; then \
	  printf '  no serial port found  ·  plug the J-Link (J9) in, or pass\n' >&2; \
	  printf '  OTA_SERIAL=/dev/cu.usbmodemXXXX  ·  `ls /dev/cu.usbmodem*` lists them\n' >&2; \
	  exit 1; \
	fi

ota-smp-push: $(CDK_OTA_PY)
	@test -f '$(CDK_PATCH)' || { printf '  no patch at %s  ·  run `make fota`\n' '$(CDK_PATCH)' >&2; exit 1; }
	@if [ -n '$(OTA_SERIAL)' ]; then \
	  $(CDK_PORT_RESOLVE); \
	  $(CDK_OTA_PY) $(REPO_ROOT)/scripts/ultrawidelock_smp.py '$(CDK_PATCH)' \
	    --serial "$$port" || exit 1; \
	else \
	  $(CDK_OTA_PY) $(REPO_ROOT)/scripts/ultrawidelock_smp.py '$(CDK_PATCH)' \
	    $(if $(OTA_NAME),--name '$(OTA_NAME)') || exit 1; \
	fi
	@mkdir -p '$(dir $(CDK_DEPLOYED))' && cp '$(CDK_SIGNED_HEX)' '$(CDK_DEPLOYED)'
	@cp '$(CDK_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.elf' '$(CDK_DEPLOYED_ELF)' 2>/dev/null || true
	@printf '  recorded as deployed  ·  %s\n' '$(CDK_DEPLOYED)'

## ota-smp-list: print what the board is running  ·  OTA_SERIAL=auto for the cable
ota-smp-list: $(CDK_OTA_PY)
	@if [ -n '$(OTA_SERIAL)' ]; then \
	  $(CDK_PORT_RESOLVE); \
	  $(CDK_OTA_PY) $(REPO_ROOT)/scripts/ultrawidelock_smp.py --list --serial "$$port"; \
	else \
	  $(CDK_OTA_PY) $(REPO_ROOT)/scripts/ultrawidelock_smp.py --list \
	    $(if $(OTA_NAME),--name '$(OTA_NAME)'); \
	fi

## ota-recovery: enter MCUboot serial recovery over SWD instead of holding SW2
##   Then talk to it with:  make ota-smp-list OTA_SERIAL=auto
#
# WHY THIS EXISTS, and it is not convenience. CDK-16 -- serial recovery having
# worked exactly once -- has two candidate halves that nobody has been able to
# separate: the ENTRY (a 5 s button hold, which writes Zephyr's retained boot
# mode and warm-resets) and the SERIAL (MCUboot answering once it is there).
# Every test so far exercised both at once, so a failure never said which.
#
# This is the entry, done by the probe. It writes exactly what the application
# writes -- BOOT_MODE_TYPE_BOOTLOADER, which is 0x01
# (zephyr/include/zephyr/retention/bootmode.h:34-38), into GPREGRET2 at
# 0x40000520 (zephyr/dts/arm/nordic/nrf52833.dtsi:93-98, reg = <0x40000520 1>)
# -- and then resets. nRF52 clears GPREGRET only on power-on and brownout
# reset, so a debugger reset retains it, which is what makes this work at all.
#
# MCUboot consumes and clears the byte on the boot that reads it, so this is
# one-shot exactly like the button: reset again afterwards and the application
# boots normally. Nothing is written to flash by this target.
ota-recovery:
	@printf '  writing BOOT_MODE_TYPE_BOOTLOADER to GPREGRET2 (0x40000520)\n'
	@probe-rs write --chip $(CDK_CHIP) $(CDK_PROBE_ARG) b8 0x40000520 1
	@probe-rs reset --chip $(CDK_CHIP) $(CDK_PROBE_ARG)
	@printf '  reset. MCUboot should now be in serial recovery, and the\n'
	@printf '  application should NOT be running -- which is the check:\n\n'
	@printf '    make ota-smp-list                  # BLE. Finding a board here means\n'
	@printf '                                       # the app booted, so recovery was\n'
	@printf '                                       # never entered.\n'
	@printf '    make ota-smp-list OTA_SERIAL=auto  # the cable. An answer here is\n'
	@printf '                                       # MCUboot talking.\n\n'

## ota-window: open the update window over SWD instead of pressing SW2
ota-window:
	@elf='$(CDK_DEPLOYED_ELF)'; \
	[ -f "$$elf" ] || elf='$(CDK_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.elf'; \
	nm=$$(ls /opt/nordic/ncs/toolchains/*/opt/zephyr-sdk/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm 2>/dev/null | head -1); \
	addr=$$($$nm "$$elf" | awk '$$3 ~ /^s_open(\.|$$)/ { print $$1; exit }'); \
	if [ -z "$$addr" ]; then printf '  cannot find s_open in %s\n' "$$elf" >&2; exit 1; fi; \
	printf '  opening the update window by writing s_open at 0x%s\n' "$$addr"; \
	probe-rs write --chip $(CDK_CHIP) $(CDK_PROBE_ARG) b8 "0x$$addr" 1

## release: build and bundle the image to publish  ·  needs RELEASE_KEY=<path>
CDK_RELEASE_BUILD ?= $(ULTRAWIDELOCK_BUILD_ROOT)/cdk-release
CDK_RELEASE_OUT   ?= $(ULTRAWIDELOCK_BUILD_ROOT)/release/ultrawidelock-dwm3001cdk
CDK_RELEASE_BUILD := $(abspath $(CDK_RELEASE_BUILD))
CDK_RELEASE_OUT   := $(abspath $(CDK_RELEASE_OUT))
CDK_RELEASE_VER   ?= $(shell git -C $(REPO_ROOT) describe --tags --always --dirty 2>/dev/null || echo unknown)
# Exported for the same reason as ESP_RELEASE_VER in mk/esp32.mk: a tag reaches
# the recipe through the environment rather than being pasted into '...' at Make
# time, where an apostrophe in it would end the quoting.
export CDK_RELEASE_VER

release:
	@if [ -z '$(RELEASE_KEY)' ]; then \
	  printf '  RELEASE_KEY is not set.\n' >&2; \
	  printf '  The release image is signed with your OFFLINE key, and there is no default:\n' >&2; \
	  printf '  MCUboot embeds its public half permanently, so this decides what every board\n' >&2; \
	  printf '  flashed from this release will ever accept over the air.\n\n' >&2; \
	  printf '    make release RELEASE_KEY=/path/to/ultrawidelock-release.pem\n\n' >&2; \
	  printf '  No key yet?  openssl ecparam -name prime256v1 -genkey -noout -out <path>\n' >&2; \
	  printf '  Then back it up. Losing it means no released board can be updated again.\n' >&2; \
	  exit 1; \
	fi
	@test -f '$(RELEASE_KEY)' || { printf '  no such key: %s\n' '$(RELEASE_KEY)' >&2; exit 1; }
	@if [ "$$(cd $(dir $(RELEASE_KEY)) 2>/dev/null && pwd)/$(notdir $(RELEASE_KEY))" = '$(CDK_KEY)' ]; then \
	  printf '  that is this checkout dev key (%s).\n' '$(CDK_KEY)' >&2; \
	  printf '  It is gitignored, per-clone and regenerated freely, so signing a published\n' >&2; \
	  printf '  image with it strands every board that installs one. Use the offline key.\n' >&2; \
	  exit 1; \
	fi
	@$(MAKE) --no-print-directory build PRISTINE=1 SMP=1 RELEASE=1 \
	  CDK_BUILD='$(CDK_RELEASE_BUILD)' CDK_KEY='$(abspath $(RELEASE_KEY))'
	@# The setup code is read back out of the build rather than assumed: it is
	@# generated at configure time and merged into the hex, so the only honest
	@# source for what a user must type is the image that was just produced.
	@code=$$(python3 $(REPO_ROOT)/scripts/spake2p_verifier.py \
	    --from-config '$(CDK_RELEASE_BUILD)/$(CDK_IMAGE)/zephyr/.config' \
	  | awk '/setup code/ { print $$3 }'); \
	  test -n "$$code" || { printf '  could not read the setup code back\n' >&2; exit 1; }; \
	  $(REPO_ROOT)/scripts/release-bundle.sh \
	    --target dwm3001cdk --out '$(CDK_RELEASE_OUT)' \
	    --version "$$CDK_RELEASE_VER" \
	    --board 'DWM3001CDK (decawave_dwm3001cdk, nRF52833)' \
	    --setup-code "$$code" \
	    --commission-note 'Type this into Apple Home. There is no QR label on this board.' \
	    '$(CDK_RELEASE_BUILD)/merged.hex' \
	    '$(CDK_RELEASE_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.signed.hex'
	@printf '  Zip it and attach it to the release:\n'
	@printf '    (cd %s && zip -qr ../ultrawidelock-dwm3001cdk.zip ultrawidelock-dwm3001cdk)\n\n' '$(dir $(CDK_RELEASE_OUT))'

# zephyr.signed.hex ships BESIDE merged.hex, and it is not a duplicate of it.
#
# merged.hex is what a J-Link writes: bootloader at 0x0 and the signed app after
# it. A delta cannot be built from that -- ultrawidelock_patch.py reads MCUboot
# image headers, and the first thing in a merged hex is not an MCUboot image.
#
# So the fan below needs the bare signed app of EVERY release still in the
# field, and the only moment that file provably exists is the release build that
# produced it. Not shipping it means that once a version is published, no future
# release can ever build an update for the boards running it: they are stranded
# on a J-Link forever. It costs ~250 KB in the bundle.

## ota-fan: build one delta per released image, the whole image, and the index
##   PREV_HEXES='a.hex b.hex ...'  zephyr.signed.hex of every release in the field
##
##   Run AFTER `make release`, with the same RELEASE_KEY: the deltas are signed
##   with it and every board checks that signature before it writes anything.
##
##   The whole signed image goes out beside the deltas. It is what MCUboot's
##   serial recovery accepts, and it is the only artifact on this board that
##   needs no starting image -- so it is the only one that can rescue a board
##   whose application does not boot, and it needs no probe to do it.
##   ota-index.py refuses to publish it unless it is the image the deltas
##   converge on, because recovering a board onto a build nothing updates would
##   strand it.
CDK_OTA_DIR ?= $(ULTRAWIDELOCK_BUILD_ROOT)/release/ota
CDK_OTA_DIR := $(abspath $(CDK_OTA_DIR))

ota-fan: $(CDK_OTA_PY)
	@if [ -z '$(PREV_HEXES)' ]; then \
	  printf '  PREV_HEXES is empty, so there is nothing to build a DELTA from.\n'; \
	  printf '  On the very first release there is nothing, and that is correct --\n'; \
	  printf '  no board in the world is running an older image yet. The whole\n'; \
	  printf '  image is still published, and serial recovery still installs it,\n'; \
	  printf '  so the release does have an over-the-air path. To add deltas,\n'; \
	  printf '  pass the zephyr.signed.hex of each release still in the field:\n\n'; \
	  printf "    make ota-fan RELEASE_KEY=<key> PREV_HEXES='v0.3.0/zephyr.signed.hex ...'\n\n"; \
	fi
	@test -f '$(CDK_RELEASE_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.signed.hex' || { \
	  printf '  no release build at %s  ·  run `make release RELEASE_KEY=...` first\n' \
	    '$(CDK_RELEASE_BUILD)' >&2; exit 1; }
	@test -n '$(RELEASE_KEY)' || { printf '  RELEASE_KEY is not set.\n' >&2; exit 1; }
	@rm -rf '$(CDK_OTA_DIR)/dwm3001cdk'
	@mkdir -p '$(CDK_OTA_DIR)/dwm3001cdk'
	@to='$(CDK_RELEASE_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.signed.hex'; \
	 for prev in $(PREV_HEXES); do \
	   test -f "$$prev" || { printf '  no such image: %s\n' "$$prev" >&2; exit 1; }; \
	   printf '  delta from %s\n' "$$prev"; \
	   $(CDK_OTA_PY) $(REPO_ROOT)/scripts/ultrawidelock_patch.py build \
	     --from "$$prev" --to "$$to" --build-dir '$(CDK_RELEASE_BUILD)' \
	     --key '$(abspath $(RELEASE_KEY))' --out '$(CDK_OTA_DIR)/fan.wdfu' || exit 1; \
	   $(CDK_OTA_PY) $(REPO_ROOT)/scripts/ultrawidelock_patch.py wrap \
	     '$(CDK_OTA_DIR)/fan.wdfu' --version '$(FOTA_VERSION)' \
	     --out-dir '$(CDK_OTA_DIR)/dwm3001cdk' \
	     --from-image "$$prev" --to-image "$$to" >/dev/null || exit 1; \
	 done; \
	 rm -f '$(CDK_OTA_DIR)/fan.wdfu'
	@printf '  whole image for serial recovery\n'
	@$(CDK_OTA_PY) $(REPO_ROOT)/scripts/ultrawidelock_patch.py recovery \
	  '$(CDK_RELEASE_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.signed.hex' \
	  --out-dir '$(CDK_OTA_DIR)/dwm3001cdk' --version '$(FOTA_VERSION)' >/dev/null
	@python3 $(REPO_ROOT)/scripts/ota-index.py \
	  --out '$(CDK_OTA_DIR)/ota-index.json' \
	  --cdk-dir '$(CDK_OTA_DIR)/dwm3001cdk' \
	  --version "$$CDK_RELEASE_VER"

## ota-local: stage the CURRENT build as an index the page can serve  ·  DEV ONLY
##   Needs `make fota` first. Then `make docs-serve` and open /flash/index.html.
#
# WHY THIS IS NOT `ota-fan`. The page reads ota/ota-index.json, and until now the
# only thing that produced one was the release flow -- which wants a release
# build, an offline key and the signed .hex of every version still in the field.
# That is correct for publishing and absurd as a prerequisite for opening the
# page on localhost, and it is the reason the browser half of this feature has
# never been run against a board: not because it was hard, but because there was
# nothing for it to fetch.
#
# This assembles the same shape from whatever `make fota` just built. The
# artifacts are signed with the DEV key, so a board flashed from a release will
# refuse everything here -- which is the point of the dev key and not a fault.
# Nothing here is publishable and nothing here goes near build/release from a
# release build: it writes the same directory, so run `make ota-fan` again
# before cutting one.
ota-local: $(CDK_OTA_PY)
	@delta=$$(ls -t '$(CDK_FOTA_BUILD)'/ultrawidelock-*-to-*.zip 2>/dev/null | head -1); \
	if [ -z "$$delta" ]; then \
	  printf '  no delta in %s  ·  run `make fota` first\n' '$(CDK_FOTA_BUILD)' >&2; \
	  printf '  (a delta needs the board to be on a DIFFERENT build than the tree;\n' >&2; \
	  printf '   `make fota` says so plainly if it is not.)\n' >&2; \
	  exit 1; \
	fi; \
	rm -rf '$(CDK_OTA_DIR)/dwm3001cdk'; \
	mkdir -p '$(CDK_OTA_DIR)/dwm3001cdk'; \
	cp "$$delta" '$(CDK_OTA_DIR)/dwm3001cdk/'; \
	cp "$${delta%.zip}.bin" '$(CDK_OTA_DIR)/dwm3001cdk/'; \
	printf '  delta     %s\n' "$$(basename "$$delta")"
	@$(CDK_OTA_PY) $(REPO_ROOT)/scripts/ultrawidelock_patch.py recovery \
	  '$(CDK_FOTA_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.signed.hex' \
	  --out-dir '$(CDK_OTA_DIR)/dwm3001cdk' --version '$(IMAGE_VERSION)' >/dev/null
	@python3 $(REPO_ROOT)/scripts/ota-index.py \
	  --out '$(CDK_OTA_DIR)/ota-index.json' \
	  --cdk-dir '$(CDK_OTA_DIR)/dwm3001cdk' --version 'dev-$(IMAGE_VERSION)'
	@printf '\n  now:  make docs-serve\n'
	@printf '  then: http://localhost:8080/flash/index.html\n'
	@printf '  localhost counts as a secure context, so WebSerial and Web\n'
	@printf '  Bluetooth both work there without a certificate.\n\n'

## ota-deps: create the host virtualenv the update tooling runs in
ota-deps: $(CDK_OTA_PY)
$(CDK_OTA_PY):
	@printf '  creating the update tooling virtualenv  ·  %s\n' '$(CDK_OTA_VENV)'
	@python3 -m venv '$(CDK_OTA_VENV)'
	@'$(CDK_OTA_VENV)/bin/pip' install --quiet --disable-pip-version-check \
	  detools cryptography bleak pyserial
	@printf '  ready  ·  detools, cryptography, bleak, pyserial\n'

## dfu-serial: MCUboot serial recovery via the Go mcumgr  ·  see CDK-16, unreliable
##   For the APPLICATION over the same cable, use `make ota-smp OTA_SERIAL=auto`
##   instead -- that path is new, is not CDK-16, and does not involve mcumgr.
dfu-serial:
	@port='$(DFU_PORT)'; \
	if [ -z "$$port" ]; then port=$$(ls /dev/cu.usbmodem* 2>/dev/null | head -n1); fi; \
	if [ -z "$$port" ]; then \
	  printf '  no serial port found  ·  plug the J-Link (J9) in, or pass DFU_PORT=/dev/cu.usbmodemXXXX\n' >&2; \
	  exit 1; \
	fi; \
	$(REPO_ROOT)/scripts/cdk-dfu.sh "$$port" '$(DFU_BAUD)' '$(CDK_SIGNED)' '$(CDK_CHIP)'

## monitor: stream the DWM3001CDK's console over RTT  ·  Ctrl-C to stop
monitor:
	@command -v probe-rs >/dev/null 2>&1 || { printf '  probe-rs not found  ·  see `make tools`\n' >&2; exit 1; }
	@test -f $(CDK_RTT_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.elf || { printf '  no ELF at %s/$(CDK_IMAGE)/zephyr/zephyr.elf  ·  build it first\n' '$(CDK_RTT_BUILD)' >&2; exit 1; }
	@$(REPO_ROOT)/scripts/cdk-rtt-elf-check.sh \
	  '$(CDK_RTT_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.elf' '$(CDK_DEPLOYED_ELF)'
	@# The code you would be asked for while watching this. Never fatal: the
	@# reader build has no Matter symbols and a console is still worth having.
	@python3 $(REPO_ROOT)/scripts/spake2p_verifier.py \
	  --from-config $(CDK_RTT_BUILD)/$(CDK_IMAGE)/zephyr/.config 2>/dev/null || true
	@probe-rs attach --chip $(CDK_CHIP) $(CDK_PROBE_ARG) \
	  $(CDK_RTT_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.elf

## monitor-rtt: RTT feed for the DWM3001CDK  ·  ctrl-c ends it
##   Takes the probe over (stops any capture already attached to this chip) and
##   appends a copy of everything to build/rtt-cdk.log (LOG=path moves it).
##   Attaches with the DEPLOYED ELF -- the copy `flash` keeps of what the board
##   actually runs -- so this works without knowing which CDK_BUILD produced it.
monitor-rtt:
	@command -v probe-rs >/dev/null 2>&1 || { printf '  probe-rs not found  ·  see `make tools`\n' >&2; exit 1; }
	-@pkill -f 'probe-rs attach --chip $(CDK_CHIP)' 2>/dev/null || true; sleep 1
	@log='$(if $(LOG),$(LOG),$(ULTRAWIDELOCK_BUILD_ROOT)/rtt-cdk.log)'; \
	elf='$(CDK_DEPLOYED_ELF)'; \
	[ -f "$$elf" ] || elf='$(CDK_RTT_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.elf'; \
	[ -f "$$elf" ] || { printf '  no ELF (deployed or in %s)  ·  build/flash first\n' '$(CDK_RTT_BUILD)' >&2; exit 1; }; \
	printf '  RTT: DWM3001CDK  ·  elf %s  ·  copy -> %s  ·  ctrl-c ends\n' "$$elf" "$$log"; \
	exec script -aq "$$log" probe-rs attach --chip $(CDK_CHIP) $(CDK_PROBE_ARG) "$$elf"

## cdk-size: what the image costs and how much room is left  ·  measures only
cdk-size:
	@test -f '$(CDK_BUILD)/$(CDK_IMAGE)/zephyr/zephyr.elf' || { \
	  printf '  no ELF at %s/%s/zephyr/zephyr.elf  ·  run `make build` first\n' \
	    '$(CDK_BUILD)' '$(CDK_IMAGE)' >&2; exit 2; }
	@# From ./workspace like every other recipe here, so the west invocation the
	@# reports need resolves its manifest. Every path handed to the script is
	@# already absolute, so the working directory does not reach the output.
	@cd $(REPO_ROOT)/workspace 2>/dev/null || cd $(REPO_ROOT); \
	 python3 $(REPO_ROOT)/scripts/cdk-size.py $(CDK_SIZE_ARGS) \
	   $(if $(GITHUB_STEP_SUMMARY),--summary '$(GITHUB_STEP_SUMMARY)')
	@printf '  report  ·  %s\n\n' '$(CDK_SIZE_JSON)'

## cdk-size-check: fail if the image lost headroom against the recorded baseline
cdk-size-check:
	@# Suppressed for the measure step: the comparison table below carries the
	@# same numbers with the baseline beside them, and two tables in one step
	@# summary is how a reader ends up reading the wrong one.
	@$(MAKE) --no-print-directory cdk-size GITHUB_STEP_SUMMARY=
	@python3 $(REPO_ROOT)/scripts/cdk-size-compare.py \
	  --baseline '$(CDK_SIZE_BASELINE)' --current '$(CDK_SIZE_JSON)' \
	  $(if $(GITHUB_STEP_SUMMARY),--summary '$(GITHUB_STEP_SUMMARY)')

## cdk-size-baseline: record the current tree as the baseline to compare against
cdk-size-baseline: cdk-size
	@python3 $(REPO_ROOT)/scripts/cdk-size-baseline.py \
	  --from '$(CDK_SIZE_JSON)' --out '$(CDK_SIZE_BASELINE)'

# ---- compatibility aliases ---------------------------------------------------
# The names these targets had while the CDK still lived under ports/. Each does
# the work and prints where it moved to, so a bookmarked command keeps working
# and says so once. Not in the help list: the new names are above.
cdk-ultrawidelock-matter-thread:
	@printf '  cdk-ultrawidelock-matter-thread is now `make build`\n'
	@$(MAKE) --no-print-directory build

cdk-reader:
	@printf '  cdk-reader is now `make reader`\n'
	@$(MAKE) --no-print-directory reader

cdk-flash:
	@printf '  cdk-flash is now `make flash`\n'
	@$(MAKE) --no-print-directory flash

cdk-flash-erase:
	@printf '  cdk-flash-erase is now `make flash-erase`\n'
	@$(MAKE) --no-print-directory flash-erase

cdk-rtt:
	@printf '  cdk-rtt is now `make monitor`\n'
	@$(MAKE) --no-print-directory monitor
