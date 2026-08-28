# Zephyr anchor example

A two-anchor double-sided two-way-ranging bench, separate from the lock
applications and changing nothing about what either board boots as a lock.

Build one role at a time:

```sh
make anchor-build ROLE=initiator ANCHOR_BOARD=nrf5340dk/nrf5340/cpuapp
make anchor-build ROLE=responder ANCHOR_BOARD=decawave_dwm3001cdk
```

Or both halves at once:

```sh
make anchor-pair
```

`ROLE` is `initiator` or `responder`. `ANT_DLY=<dtu>` supplies the calibrated
lumped antenna delay; omitting it leaves the pair uncalibrated. `make
anchor-flash` and `make anchor-monitor` take the same `ROLE` and `ANCHOR_BOARD`
as the build.
