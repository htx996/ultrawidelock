# BLE witness — nRF52840 dongle, one image, no unlock authority.
#
# Hears every advertiser near the door, labels each one under a key the lock
# does not hold, ranks them by strength, and sends a sealed window to the lock
# over Thread every 2 s. It never decides anything: the lock works out which
# label is the phone, and the lock alone opens the door.
#
# ONE IMAGE for every mounting position. Role, keys and the Thread dataset are
# provisioned once, over USB, before the dongle goes on the wall:
#
#   make witness-build
#   make witness-flash
#   make witness-prov-help          # prints the exact PROV line to paste
#
# Placement: one dongle each side of the door, mirrored across the door plane.
# The comparison the lock makes is inside-versus-outside for the same
# advertiser, so a mounting that is not mirrored tilts the plane the lock
# thinks it is measuring. See modules/ultrawidelock_anchor/include/
# ultrawidelock_fusion.h on why no code substitutes for that.
#
# LED: fast blink = not provisioned; slow blink = provisioned, Thread not
# attached; solid = attached and reporting. Once the dongle is on the wall the
# LED is the only diagnostic, so it says which of the three it is.
#
# What this firmware does NOT do, and used to:
#   - no LEARN pass. Nothing is fingerprinted, so nothing needs teaching.
#   - no ADDR filter. The lock cannot tell a witness which advertiser to watch,
#     because the address it holds from the credential connection is not the
#     address this board hears. See ultrawidelock_witness_pick.h.
#   - no per-role build. Role is provisioned.
#   - no Raspberry Pi, no J-Link, no UART summaries to a host.
#
# THE DIAGNOSTIC OVERLAYS. The nine overlay-* files beside this one -- seven
# .conf and two devicetree halves -- are not build profiles. They are the
# bisection that found why this image would not boot, kept because the reasoning
# in them is worth more than the disk. The answer is in sysbuild.conf: Partition
# Manager had put settings_storage on the Nordic USB bootloader's ACL-protected
# MBR params page, and the first NVS erase became a precise bus fault. Every
# overlay here predates that fix, and most record a suspect that was cleared --
# CryptoCell, Bluetooth, USB, the 802.15.4 driver, the boot banners. Read one
# before re-running it: several say outright that their own measurement is void.
#
# They are layered by hand, never by a target:
#
#   make witness-build TRACE=1 \
#     WITNESS_CONF="overlay-otmain.conf;overlay-nobt.conf" \
#     WITNESS_DTC="overlay-nocc3xx.overlay"
#
# TRACE=1 adds the boot-milestone LEDs. Two come in pairs and are useless apart:
# overlay-nocc3xx.conf needs its .overlay or the entropy device dangles, and
# overlay-uartcon.conf needs its .overlay to move the console off USB. The two
# that take a radio out -- overlay-nobt.conf and overlay-noradio.conf -- cannot
# witness anything at all, and their only output is the boot trace. The rest
# change when or where the boot reports, not what the image does.
#
# Design: docs/inside-latch.md
