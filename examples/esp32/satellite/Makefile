# Makefile — forwarder for the ESP32 satellite responder.
#
# The recipes live in mk/esp32.mk at the repo root, so the serial-port guard and
# the one build root exist in one place. This file only supplies APP=:
#
#   make build                 ->  make -C <root> esp-build   APP=satellite
#   make flash                 ->  make -C <root> esp-flash   APP=satellite
#   make monitor               ->  make -C <root> esp-monitor APP=satellite
#
# Running them from the repo root as `make esp-<target> APP=satellite` is the
# same thing and is what the top-level `make help` lists.

APP  := satellite
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../..)

.DEFAULT_GOAL := help

.PHONY: help
help:
	@$(MAKE) --no-print-directory -C $(ROOT) esp-help APP=$(APP)

.PHONY: force
force: ;
Makefile: ;
%: force
	@$(MAKE) --no-print-directory -C $(ROOT) esp-$@ APP=$(APP)
