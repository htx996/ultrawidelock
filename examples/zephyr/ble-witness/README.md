# BLE witness

One nRF52840 dongle image for both sides of the door. It hears every advertiser
near the door, labels each one under a key the lock does not hold, ranks them by
strength, and sends a sealed window to the lock over Thread every 2 s. The lock
works out which label is the phone, and the lock alone opens the door.

Design: [`docs/inside-latch.md`](../../../docs/inside-latch.md).

## Install

Role, keys and the Thread dataset are provisioned once, over USB, before the
dongle goes on the wall.

```sh
make witness-build
make witness-flash
make witness-prov-help          # prints the exact PROV line to paste
```

Mount one dongle each side of the door, mirrored across the door plane. The lock
compares inside against outside for the same advertiser, so a mounting that is
not mirrored tilts the plane it thinks it is measuring
(`modules/ultrawidelock_anchor/include/ultrawidelock_fusion.h`).

| LED | State |
|---|---|
| Fast blink | not provisioned |
| Slow blink | provisioned, Thread not attached |
| Solid | attached and reporting |

On the wall, the LED is the only diagnostic.

## Diagnostic overlays

The nine `overlay-*` files beside this one, seven `.conf` and two devicetree
halves, are the bisection that found why this image would not boot. The answer is
in `sysbuild.conf`: Partition Manager had put `settings_storage` on the Nordic USB
bootloader's ACL-protected MBR params page, and the first NVS erase became a
precise bus fault.

> [!WARNING]
> Every overlay here predates that fix, and most record a suspect that was
> cleared: CryptoCell, Bluetooth, USB, the 802.15.4 driver, the boot banners.
> Several say outright that their own measurement is void. Read one before
> re-running it.

They are layered by hand, never by a target:

```sh
make witness-build TRACE=1 \
  WITNESS_CONF="overlay-otmain.conf;overlay-nobt.conf" \
  WITNESS_DTC="overlay-nocc3xx.overlay"
```

`TRACE=1` adds the boot-milestone LEDs. Two overlays come in pairs:
`overlay-nocc3xx.conf` needs its `.overlay` or the entropy device dangles, and
`overlay-uartcon.conf` needs its `.overlay` to move the console off USB.
`overlay-nobt.conf` and `overlay-noradio.conf` each take a radio out, so their
only output is the boot trace. The rest change when or where the boot reports.
