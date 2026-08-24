# mk/esp32.mk — the ESP32 ports: matter-lock, reader, initiator.
#
# One implementation for all three apps, selected by APP=. The app Makefiles
# under apps/ and examples/esp32/ are thin forwarders into this file, so every command
# their READMEs document keeps working and the serial-port guard below exists in
# exactly one place.
#
#   make esp-build APP=matter-lock TARGET=esp32s3
#   make esp-flash APP=reader
#   cd examples/esp32/reader && make flash       # same thing, via the forwarder
#
# No ESP-IDF or esp-matter version is pinned anywhere in the repo; the paths
# below are the only thing this file fixes, and both are overridable. What
# `make esp-bootstrap` installs on a machine that has neither is the pair the
# bench builds against — a record of what is known to work, not a requirement
# the build enforces. It is written down once, in scripts/esp-bootstrap.sh, and
# `make esp-env` prints it.

APP     ?= matter-lock
TARGET  ?= esp32s3
# VARIANT layers an extra sdkconfig fragment and, because it is part of the
# build directory name, gets its own tree and its own sdkconfig. That is the
# whole point: switching variants used to mean reconfiguring one directory, so a
# build that once passed a fragment kept using it and dropping the variant
# silently kept producing the variant image.
#   presence  the signed-presence console command   (matter-lock, reader)
#   hamqtt    Home Assistant MQTT + mbedTLS sizing  (matter-lock)
#   piv       PIN-enforced PIV recovery build       (matter-lock)
VARIANT ?=

ifeq ($(APP),matter-lock)
ESP_APP_DIR := $(REPO_ROOT)/apps/esp32-matter-lock
else
ESP_APP_DIR := $(REPO_ROOT)/examples/esp32/$(APP)
endif
ESP_BUILD   := $(ULTRAWIDELOCK_BUILD_ROOT)/esp32-$(APP)-$(TARGET)$(if $(VARIANT),-$(VARIANT))
# The generated config belongs to the build, not to the checkout. Keyed by
# APP+TARGET+VARIANT along with the tree, so building the C6 can no longer
# overwrite the config the hardware-validated S3 image was built from.
ESP_SDKCONFIG := $(ESP_BUILD)/sdkconfig

# ESP-IDF + esp-matter environment. Override if installed elsewhere:
#   make esp-build IDF_EXPORT=/path/to/esp-idf/export.sh
IDF_EXPORT      ?= $(HOME)/esp/esp-idf/export.sh
ESP_MATTER_PATH ?= $(HOME)/esp/esp-matter
# make esp-flash FORCE=1 stops whatever holds the UART (a monitor/tio) first.
FORCE ?=
# Only matter-lock needs esp-matter; sourcing it for the others would demand an
# install they do not use. Both exports are noisy on success, so their output is
# dropped; failures still show because the idf.py call after them fails loudly.
ifeq ($(APP),matter-lock)
ESP_ENV := export ESP_MATTER_PATH="$(ESP_MATTER_PATH)" && . "$(IDF_EXPORT)" >/dev/null 2>&1 && \
           . "$(ESP_MATTER_PATH)/export.sh" >/dev/null 2>&1
else
ESP_ENV := . "$(IDF_EXPORT)" >/dev/null 2>&1
endif

# Every build shares one directory and one sdkconfig, named explicitly so a
# recipe can never pick up whichever config happened to be in the app folder.
# ESP-IDF appends sdkconfig.defaults.$(TARGET) to each entry by itself, so the
# per-target fragments ride along without being listed.
ESP_DEFAULTS := sdkconfig.defaults$(if $(VARIANT),;sdkconfig.$(VARIANT))
ifeq ($(VARIANT),hamqtt)
ESP_DEFAULTS := sdkconfig.defaults;sdkconfig.defaults.hamqtt
endif

# IDF_TARGET is passed on EVERY invocation, not left to a prior `set-target`.
# The generated config lives in the build directory now, so a fresh directory
# starts with no target selected and ESP-IDF silently falls back to plain esp32 --
# which fails deep in the compile as
#   board_pins.h: #error "Unsupported ESP32 target for the DW3000 port"
# rather than as "you forgot set-target". Passing it here makes
# `make esp-build APP=... TARGET=...` correct on its own. Safe to repeat: the
# directory name already encodes the target, so it can never contradict itself.
IDFPY := $(ESP_ENV) && idf.py -B "$(ESP_BUILD)" \
         -D IDF_TARGET="$(TARGET)" \
         -D SDKCONFIG="$(ESP_SDKCONFIG)" \
         -D SDKCONFIG_DEFAULTS="$(ESP_DEFAULTS)"

# USB vendor ids (decimal, as ioreg reports them) used to classify ports.
# Keep values trailing-whitespace-free: an inline comment would leak spaces into
# the value and break the awk string compares below.
# 0x1A86 WCH CH343/CH9102 UART bridge (the ESP devkit); 0x303A Espressif native
# USB (S3 USB-OTG port); 0x1366 SEGGER J-Link = the nRF5340DK, NEVER flash.
VID_WCH       := 6790
VID_ESPRESSIF := 12346
VID_SEGGER    := 4966

# Resolve $$port to a SAFE ESP port, or fail loudly. Refuses SEGGER ports.
define RESOLVE_PORT
port='$(PORT)'; \
table=$$(ioreg -l -w0 2>/dev/null | LC_ALL=C awk '/"idVendor"/{v=$$0;sub(/.*= /,"",v);vend=v} /"IOCalloutDevice"/&&/usbmodem/{c=$$0;sub(/.*= "/,"",c);sub(/".*/,"",c);print vend"\t"c}'); \
if [ -n "$$port" ]; then \
	vend=$$(printf '%s\n' "$$table" | awk -F'\t' -v p="$$port" '$$2==p{print $$1}'); \
	if [ "$$vend" = "$(VID_SEGGER)" ]; then echo "  refusing $$port: SEGGER/J-Link (nRF) port — protected rig" >&2; exit 1; fi; \
else \
	port=$$(printf '%s\n' "$$table" | awk -F'\t' '$$1=="$(VID_WCH)"||$$1=="$(VID_ESPRESSIF)"{print $$2; exit}'); \
	if [ -z "$$port" ]; then \
		if printf '%s\n' "$$table" | awk -F'\t' '$$1=="$(VID_SEGGER)"{f=1} END{exit f?0:1}'; then \
			echo "  only nRF J-Link (SEGGER) ports present — plug the ESP into its UART port, or pass PORT=" >&2; \
		else \
			echo "  no ESP serial port found — plug it in or pass PORT=/dev/cu.usbmodemXXXX" >&2; \
		fi; \
		ls /dev/cu.usbmodem* 2>/dev/null | sed 's/^/    seen: /' >&2; \
		exit 1; \
	fi; \
fi
endef

# Flashing needs the UART exclusively. If a monitor/tio holds $$port, refuse with
# a clear message (FORCE=1 stops the holder first). Non-destructive by default.
define ENSURE_PORT_FREE
pids=$$(lsof -t "$$port" 2>/dev/null | tr '\n' ' '); \
busy="$$pids"; \
if [ -z "$$busy" ] && command -v python3 >/dev/null 2>&1; then \
	python3 -c "import fcntl,os,sys;fd=os.open(sys.argv[1],os.O_RDWR|os.O_NONBLOCK);fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)" "$$port" >/dev/null 2>&1 \
		|| busy="held under an exclusive lock (owner not named by lsof)"; \
fi; \
if [ -n "$$busy" ]; then \
	if [ "$(FORCE)" = "1" ] && [ -n "$$pids" ]; then \
		echo "  FORCE: stopping port holder(s): $$pids"; \
		kill $$pids 2>/dev/null || true; \
		sleep 1; \
	elif [ "$(FORCE)" = "1" ]; then \
		echo "  FORCE: $$port is $$busy and has no PID to stop; trying anyway" >&2; \
	else \
		echo "  $$port is busy: $$busy" >&2; \
		echo "  quit it (idf.py monitor: ctrl-]  ·  tio: ctrl-t q) or run: make esp-flash FORCE=1" >&2; \
		exit 1; \
	fi; \
fi
endef

# Fail before idf.py does, naming the variable that fixes it.
define ESP_CHECK_ENV
@[ -d "$(ESP_APP_DIR)" ] || { echo "  no such app: APP=$(APP)  ·  try matter-lock, reader, satellite or initiator" >&2; exit 1; }
@[ -f "$(IDF_EXPORT)" ] || { echo "  ESP-IDF export.sh not found at $(IDF_EXPORT)" >&2; \
  echo "  install it:  make esp-bootstrap APP=$(APP)" >&2; \
  echo "  or set it:   make <target> IDF_EXPORT=/path/to/esp-idf/export.sh" >&2; exit 1; }
endef
ifeq ($(APP),matter-lock)
define ESP_CHECK_MATTER
@[ -f "$(ESP_MATTER_PATH)/export.sh" ] || { echo "  esp-matter not found at $(ESP_MATTER_PATH)" >&2; \
  echo "  install it:  make esp-bootstrap" >&2; \
  echo "  or set it:   make <target> ESP_MATTER_PATH=/path/to/esp-matter" >&2; exit 1; }
@[ -d "$(ESP_MATTER_PATH)/connectedhomeip/connectedhomeip/scripts" ] || { \
  echo "  esp-matter at $(ESP_MATTER_PATH) is cloned but not finished" >&2; \
  echo "  its connectedhomeip submodule is empty, so this build would fail deep inside cmake" >&2; \
  echo "  finish it:  make esp-bootstrap" >&2; exit 1; }
endef
else
ESP_CHECK_MATTER :=
endif

# Publishing.
ESP_RELEASE_CHIPS ?= esp32s3 esp32c5 esp32c6
ESP_RELEASE_OUT   ?= $(ULTRAWIDELOCK_BUILD_ROOT)/release/ultrawidelock-esp32-matter-lock
ESP_RELEASE_OUT   := $(abspath $(ESP_RELEASE_OUT))
ESP_RELEASE_STAGE := $(ULTRAWIDELOCK_BUILD_ROOT)/release/.esp-stage
ESP_RELEASE_VER   ?= $(shell git -C $(REPO_ROOT) describe --tags --always --dirty 2>/dev/null || echo unknown)
# Exported so the recipes below can read it as "$$ESP_RELEASE_VER" instead of
# pasting it in at Make time. A tag is attacker-influenced text: interpolated
# into '...' a single quote in it ends the quoting and the rest becomes shell.
# Export, not plain reference, because Make does not put ordinary variables in a
# recipe's environment -- in CI the workflow sets it and ?= keeps that value, on
# a bench `git describe` fills it, and both then reach the shell the same way.
export ESP_RELEASE_VER

.PHONY: esp-size-report esp-size-check esp-size-baseline
.PHONY: esp-bootstrap esp-check-env esp-set-target esp-build esp-rebuild esp-reconfigure \
        esp-merge-bin esp-release \
        esp-menuconfig esp-size esp-flash esp-app-flash esp-flash-erase \
        esp-monitor esp-go esp-run esp-term esp-lab esp-ports esp-clean esp-env \
        esp-presence-on esp-presence-off esp-presence-flash esp-help

esp-check-env:
	$(ESP_CHECK_ENV)
	$(ESP_CHECK_MATTER)

##@ ESP32  ·  APP=matter-lock|reader|satellite|initiator  TARGET=esp32s3|esp32c5|esp32c6
## esp-bootstrap: install ESP-IDF (+ esp-matter for matter-lock)  ·  once per machine
##   Asks before each stage. ESP-IDF alone is ~5 GB and covers reader, satellite
##   and initiator; matter-lock additionally needs esp-matter, ~15 GB and an hour.
##   Options: APP= decides whether esp-matter is offered (ESP_MATTER=1/0 overrides)
##            ESP_TARGETS=esp32s3 which chip toolchains to fetch (all = every chip)
##            IDF_VER= ESP_MATTER_REV= install something other than the bench pair
##            ESP_HOST_TOOLS=1 also build chip-tool and chip-cert (tens of minutes)
##            SETUP_AUTO=1 yes to every stage without asking (0 = never)
esp-bootstrap:
	@APP='$(APP)' IDF_EXPORT='$(IDF_EXPORT)' ESP_MATTER_PATH='$(ESP_MATTER_PATH)' \
	  $(REPO_ROOT)/scripts/esp-bootstrap.sh

## esp-set-target: regenerate this build's sdkconfig from scratch for TARGET
esp-set-target: esp-check-env
	@cd "$(ESP_APP_DIR)" && $(IDFPY) set-target $(TARGET)

## esp-build: compile the app   -> build/esp32-<app>-<target>[-<variant>]
esp-build: esp-check-env
	@cd "$(ESP_APP_DIR)" && $(IDFPY) build

## esp-rebuild: clean, then a full rebuild
esp-rebuild: esp-check-env
	@cd "$(ESP_APP_DIR)" && $(IDFPY) fullclean && $(IDFPY) build

## esp-flash: build + write bootloader + partition table + app
esp-flash: esp-check-env
	@$(RESOLVE_PORT); \
	$(ENSURE_PORT_FREE); \
	echo "  flashing $$port"; \
	cd "$(ESP_APP_DIR)" && $(IDFPY) -p "$$port" flash

## esp-app-flash: build + write only the app  (fast iteration)
esp-app-flash: esp-check-env
	@$(RESOLVE_PORT); \
	$(ENSURE_PORT_FREE); \
	echo "  app-flashing $$port"; \
	cd "$(ESP_APP_DIR)" && $(IDFPY) -p "$$port" app-flash

## esp-flash-erase: full chip erase, then flash  (drops commissioning + credential NVS)
esp-flash-erase: esp-check-env
	@$(RESOLVE_PORT); \
	$(ENSURE_PORT_FREE); \
	echo "  erasing + flashing $$port"; \
	cd "$(ESP_APP_DIR)" && $(IDFPY) -p "$$port" erase-flash && $(IDFPY) -p "$$port" flash

## esp-monitor: interactive serial console (logs + shell)  ·  ctrl-] to quit
esp-monitor: esp-check-env
	@$(RESOLVE_PORT); \
	echo "  monitoring $$port @ $(BAUD)  (ctrl-] quits)"; \
	cd "$(ESP_APP_DIR)" && $(IDFPY) -p "$$port" -b $(BAUD) monitor

## esp-go: build + flash + monitor in one shot  (the usual bench loop)
esp-go: esp-check-env
	@$(RESOLVE_PORT); \
	$(ENSURE_PORT_FREE); \
	echo "  build + flash + monitor on $$port"; \
	cd "$(ESP_APP_DIR)" && $(IDFPY) -p "$$port" -b $(BAUD) flash monitor

## esp-run: alias for esp-go
esp-run: esp-go

## esp-menuconfig: interactive sdkconfig editor
esp-menuconfig: esp-check-env
	@cd "$(ESP_APP_DIR)" && $(IDFPY) menuconfig

## esp-reconfigure: re-run cmake after editing CMakeLists / sdkconfig / adding files
esp-reconfigure: esp-check-env
	@cd "$(ESP_APP_DIR)" && $(IDFPY) reconfigure

## esp-size: binary size + partition headroom
esp-size: esp-check-env
	@cd "$(ESP_APP_DIR)" && $(IDFPY) size

# The record-and-gate pair, same tool and schema as the CDK. Unlike esp-size
# this needs NO IDF environment: the ELF and its GNU ld map are already in the
# build tree, and scripts/cdk-size.py takes them via --elf/--map because an
# ESP-IDF tree is not sysbuild-shaped. The baseline is per-app and per-target:
# an S3 and a C6 image are different builds with different budgets.
ESP_SIZE_JSON     ?= $(ESP_BUILD)/size-report.json
ESP_SIZE_BASELINE ?= $(ESP_APP_DIR)/size-baseline-$(TARGET)$(if $(VARIANT),-$(VARIANT)).json

## esp-size-report: the machine-readable size record  ·  measures only, no IDF env
esp-size-report:
	@elf=$$(ls '$(ESP_BUILD)'/*.elf 2>/dev/null | head -n1); \
	test -n "$$elf" || { \
	  printf '  no ELF under %s  ·  run `make esp-build` first\n' '$(ESP_BUILD)' >&2; exit 2; }; \
	python3 $(REPO_ROOT)/scripts/cdk-size.py \
	  --build '$(ESP_BUILD)' --image "$$(basename $${elf%.elf})" \
	  --elf "$$elf" --json '$(ESP_SIZE_JSON)' \
	  $(if $(GITHUB_STEP_SUMMARY),--summary '$(GITHUB_STEP_SUMMARY)')
	@printf '  report  ·  %s\n\n' '$(ESP_SIZE_JSON)'

## esp-size-check: fail if the image lost headroom against the recorded baseline
esp-size-check:
	@$(MAKE) --no-print-directory esp-size-report GITHUB_STEP_SUMMARY=
	@python3 $(REPO_ROOT)/scripts/cdk-size-compare.py \
	  --baseline '$(ESP_SIZE_BASELINE)' --current '$(ESP_SIZE_JSON)' \
	  $(if $(GITHUB_STEP_SUMMARY),--summary '$(GITHUB_STEP_SUMMARY)')

## esp-size-baseline: record the current tree as the baseline to compare against
esp-size-baseline: esp-size-report
	@python3 $(REPO_ROOT)/scripts/cdk-size-baseline.py \
	  --from '$(ESP_SIZE_JSON)' --out '$(ESP_SIZE_BASELINE)'

## esp-merge-bin: fuse bootloader + partition table + app into one 0x0 image
esp-merge-bin: esp-check-env
	@cd "$(ESP_APP_DIR)" && $(IDFPY) merge-bin -o ultrawidelock-$(APP)-$(TARGET).bin
	@printf '  merged  ·  %s/ultrawidelock-%s-%s.bin\n' '$(ESP_BUILD)' '$(APP)' '$(TARGET)'

## esp-release: build every chip and bundle the images to publish
esp-release:
	@for t in $(ESP_RELEASE_CHIPS); do \
	  printf '\n  building matter-lock for %s\n' "$$t"; \
	  $(MAKE) --no-print-directory esp-build APP=matter-lock TARGET="$$t" || exit 1; \
	  $(MAKE) --no-print-directory esp-merge-bin APP=matter-lock TARGET="$$t" || exit 1; \
	done
	@mkdir -p '$(ESP_RELEASE_STAGE)'
	@# The setup code is the same on every board flashed from this release: the
	@# app builds with CHIP's test parameters and no factory-data provider, so
	@# the passcode is a constant.
	@bins=; for t in $(ESP_RELEASE_CHIPS); do \
	    bins="$$bins $(ULTRAWIDELOCK_BUILD_ROOT)/esp32-matter-lock-$$t/ultrawidelock-matter-lock-$$t.bin"; \
	  done; \
	  $(REPO_ROOT)/scripts/release-bundle.sh \
	    --target esp32-matter-lock --out '$(ESP_RELEASE_OUT)' \
	    --version "$$ESP_RELEASE_VER" \
	    --board 'ESP32-S3, ESP32-C5 or ESP32-C6 + DWM3000EVB' \
	    --setup-code '3497-011-2332' \
	    --commission-note 'Type this into Apple Home. The same code, and a QR to scan instead, print on the serial console at 115200 baud.' \
	    $$bins

## esp-env: sanity-check the toolchain — prints idf.py's version and the paths
esp-env: esp-check-env
	@cd "$(ESP_APP_DIR)" && $(IDFPY) --version
	@printf '  APP             = %s\n  TARGET          = %s\n  VARIANT         = %s\n  BUILD           = %s\n  IDF_EXPORT      = %s\n  ESP_MATTER_PATH = %s\n' \
	  '$(APP)' '$(TARGET)' '$(if $(VARIANT),$(VARIANT),(none))' '$(ESP_BUILD)' '$(IDF_EXPORT)' '$(ESP_MATTER_PATH)'
	@$(REPO_ROOT)/scripts/esp-bootstrap.sh --print-pins | sed 's/^/  esp-bootstrap:  /'

## esp-term: serial console via tio  ·  no ESP backtrace decode, but robust
esp-term:
	@command -v tio >/dev/null 2>&1 || { printf '  tio not found  ·  install: brew install tio\n' >&2; exit 1; }
	@$(RESOLVE_PORT); \
	logargs=; [ -n '$(LOG)' ] && logargs='-L --log-file $(LOG)'; \
	printf '  tio %s @ %s 8N1  ·  ctrl-t q quits  ·  release before make esp-flash\n' "$$port" '$(BAUD)'; \
	exec tio -b $(BAUD) $$logargs "$$port"

## esp-ports: list attached usbmodem ports, labelled by board
esp-ports:
	@ioreg -l -w0 2>/dev/null | LC_ALL=C awk \
	  '/"idVendor"/{v=$$0;sub(/.*= /,"",v);vend=v} \
	   /"IOCalloutDevice"/&&/usbmodem/{c=$$0;sub(/.*= "/,"",c);sub(/".*/,"",c); \
	     lbl="unknown ("vend")"; \
	     if(vend=="$(VID_WCH)")lbl="ESP  (WCH UART)  <- flash target"; \
	     else if(vend=="$(VID_ESPRESSIF)")lbl="ESP  (native USB) <- flash target"; \
	     else if(vend=="$(VID_SEGGER)")lbl="nRF J-Link  ** DO NOT FLASH **"; \
	     printf "  %-34s %s\n", c, lbl}' ; \
	ls /dev/cu.usbmodem* >/dev/null 2>&1 || echo "  (no usbmodem ports attached)"

## esp-clean: remove THIS app/target/variant's build directory
esp-clean:
	@rm -rf "$(ESP_BUILD)" && printf '  removed %s\n' '$(ESP_BUILD)'

##@ ESP32 presence  (CONFIG_ULTRAWIDELOCK_PRESENCE · adds `presence` to the app's console)
## esp-presence-on: add the presence console command to this build  ·  idempotent
esp-presence-on:
	@[ -f "$(ESP_SDKCONFIG)" ] || { echo "  no sdkconfig yet — run 'make esp-build APP=$(APP)' once first" >&2; exit 1; }
	@for k in CONFIG_ULTRAWIDELOCK_PRESENCE $(if $(filter reader,$(APP)),CONFIG_ULTRAWIDELOCK_CRED_CLONE); do \
		grep -qx "$$k=y" "$(ESP_SDKCONFIG)" || printf '%s=y\n' "$$k" >> "$(ESP_SDKCONFIG)"; \
	done; echo "  presence ON  ·  next: make esp-presence-flash APP=$(APP)"

## esp-presence-off: drop the presence commands from this build
esp-presence-off:
	@[ -f "$(ESP_SDKCONFIG)" ] || { echo "  no sdkconfig yet — nothing to turn off"; exit 0; }
	@sed -i '' -e '/^CONFIG_ULTRAWIDELOCK_PRESENCE=y$$/d' -e '/^CONFIG_ULTRAWIDELOCK_CRED_CLONE=y$$/d' \
	  "$(ESP_SDKCONFIG)" && echo "  presence OFF  ·  next: make esp-flash APP=$(APP)"

## esp-presence-flash: turn presence on, then build + flash
esp-presence-flash: esp-presence-on
	@$(MAKE) --no-print-directory esp-flash

# ---- matter-lock only --------------------------------------------------------
# Recovery build with normal PIV PIN enforcement, and the walk-up profiler.
# Both are reached through VARIANT=piv / the lab target rather than a second
# build directory of their own.
.PHONY: esp-piv-pin-build esp-piv-pin-app-flash

## esp-piv-pin-build: isolated recovery build with normal PIV PIN enforcement
esp-piv-pin-build:
	@$(MAKE) --no-print-directory esp-build APP=matter-lock VARIANT=piv

## esp-piv-pin-app-flash: restore the PIN-enforced PIV app without erasing NVS
esp-piv-pin-app-flash:
	@$(MAKE) --no-print-directory esp-app-flash APP=matter-lock VARIANT=piv

## esp-lab: capture a walk-up to a timestamped log
esp-lab:
	@command -v tio >/dev/null 2>&1 || { printf '  tio not found · install: brew install tio\n' >&2; exit 1; }
	@$(RESOLVE_PORT); \
	log="walkup-$$(date +%Y%m%d-%H%M%S).log"; \
	printf '  %s @ %s · at the console: `lab on`, walk up, `lab off`, then ctrl-t q\n' "$$port" '$(BAUD)'; \
	tio -b $(BAUD) -L --log-file "$$log" "$$port" || true; \
	printf '\n  captured %s\n' "$$log"

# Help shown by the app-directory forwarders, so `cd apps/reader && make` still
# prints something useful. Lists this file's ESP rows only.
esp-help:
	@if [ -t 1 ] && [ -z "$$NO_COLOR" ]; then \
	  b=$$(printf '\033[1m'); c=$$(printf '\033[36m'); y=$$(printf '\033[1;33m'); d=$$(printf '\033[2m'); r=$$(printf '\033[0m'); \
	else b=; c=; y=; d=; r=; fi; \
	printf '\n  %sUltraWideLock ESP32%s  %s·  APP=%s TARGET=%s · run from the repo root as make esp-<target>%s\n' \
	  "$$b" "$$r" "$$d" '$(APP)' '$(TARGET)' "$$r"; \
	awk -v c="$$c" -v y="$$y" -v d="$$d" -v r="$$r" \
	  '/^##@ ESP32/ { printf "\n  %s%s%s\n", y, substr($$0,5), r; next } \
	   /^## esp-/ { s=substr($$0,4); i=index(s,": "); \
	     printf "    %s%-22s%s %s%s%s\n", c, substr(s,1,i-1), r, d, substr(s,i+2), r }' \
	  $(REPO_ROOT)/mk/esp32.mk; \
	printf '\n'
