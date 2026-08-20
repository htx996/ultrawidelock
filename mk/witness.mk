# mk/witness.mk — nRF52840 BLE witness.
#
# ONE image for every dongle. The role used to be a build flag (WITNESS_ROLE),
# which meant a distinct image per mounting position and a reflash to move one
# from inside to outside; it is provisioned at install now and lives in the
# witness's own settings. There is nothing to choose at build time.

WITNESS_APP   := $(REPO_ROOT)/examples/zephyr/ble-witness
WITNESS_BOARD ?= nrf52840dongle/nrf52840

WITNESS_BOARD_TAG := $(subst /,_,$(WITNESS_BOARD))
WITNESS_BUILD ?= $(ULTRAWIDELOCK_BUILD_ROOT)/witness-$(WITNESS_BOARD_TAG)
override WITNESS_BUILD := $(abspath $(WITNESS_BUILD))
WITNESS_PRISTINE := $(if $(PRISTINE),always,auto)

.PHONY: witness-build witness-flash witness-prov-help

##@ BLE witness  ·  one image, role provisioned at install
## witness-build: build the witness  -> build/witness-<board>
witness-build:
	@$(CDK_RUN) build -p $(WITNESS_PRISTINE) -b $(WITNESS_BOARD) \
	  -d $(WITNESS_BUILD) $(WITNESS_APP)

## witness-flash: flash the witness built for WITNESS_BOARD
witness-flash:
	@$(CDK_RUN) flash -d $(WITNESS_BUILD)

## witness-prov-help: print the one-time provisioning line to paste per dongle
witness-prov-help:
	@printf '%s\n' \
	  'Provision each dongle once, over its USB CDC console, before mounting:' \
	  '' \
	  '  PROV <role> <link-key-hex32> <group-key-hex32> <dataset-hex>' \
	  '' \
	  '  role         inside | outside | threshold   (one dongle per role)' \
	  '  link-key     16 bytes, DIFFERENT per dongle. Seals that witness'"'"'s' \
	  '               reports; the lock stores the same bytes under uwl/wit/k/<role>.' \
	  '  group-key    16 bytes, THE SAME on every dongle and NOT on the lock.' \
	  '               Labels advertisers so inside and outside can be compared.' \
	  '  dataset      the Thread active operational dataset TLVs, hex.' \
	  '               From the lock itself, which is already on the network:' \
	  '               build with overlays/thread-dataset-dump.conf appended to' \
	  '               the WHOLE default CDK_CONF list (read its header first),' \
	  '               flash, and press SW2 to open the commissioning window.' \
	  '               The dataset prints between two markers on the log.' \
	  '               `ot-ctl dataset active -x` also works if you have a node' \
	  '               with a CLI; an Apple border router does not give you one.' \
	  '' \
	  'Then SHOW to confirm, and power-cycle. Steady state needs no host:' \
	  'solid LED = attached and reporting, slow blink = provisioned but not' \
	  'attached, fast blink = not provisioned.' \
	  '' \
	  'Generate keys with:  openssl rand -hex 16' \
	  '' \
	  'THE LOCK NEEDS THE SAME LINK KEYS, and not on the image that uses' \
	  'them: the Thread build sets CONFIG_SHELL=n, so it has no console.' \
	  'Enroll on the reader image, then reflash WITHOUT erasing:' \
	  '' \
	  '  make reader && make flash CDK_BUILD=build/cdk-reader' \
	  '  # hold SW2 through reset, then on the USB console:' \
	  '  ultrawidelock witkey inside  <that dongle'"'"'s link key>' \
	  '  ultrawidelock witkey outside <that dongle'"'"'s link key>' \
	  '' \
	  '  make build LATCH=1 && make flash LATCH=1     # NOT flash-erase' \
	  '' \
	  'flash-erase takes the witness keys with everything else. See' \
	  'docs/inside-latch.md section 6.1.'
