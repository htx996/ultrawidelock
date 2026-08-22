# DWM3001CDK Lock, FreeRTOS

This sibling application is the Zephyr-free port of
`apps/dwm3001cdk-lock`. The existing Zephyr application remains the behavioral
and hardware-in-loop oracle throughout the migration.

The port includes the production FreeRTOS OSAL, radio, Thread, credential, UWB,
and optional portable Matter graph in `ports/freertos-nrf52833`. Run its host
gate with:

```sh
make freertos-port-test
```

The target image builds:

```sh
make freertos-build \
  NCS_WORKSPACE=<path-to-ncs-workspace> \
  QORVO_SDK_DIR=<path-to-extracted-DW3_QM33_SDK_1.1.1>

# Include the same portable five-fabric Matter contract as the Zephyr image.
make freertos-build FREERTOS_MATTER=ON \
  NCS_WORKSPACE=<path-to-ncs-workspace> \
  QORVO_SDK_DIR=<path-to-extracted-DW3_QM33_SDK_1.1.1>
```

The FreeRTOS Matter variant shares the portable commissioning, five-fabric,
Thread staging, selective rollback/removal, SRP, and `mf2` record code used by
the Zephyr DWM3001CDK image. Its settings backend uses the same transaction
semantics over a different on-flash log format. The behavior and power-cut
cases are host-tested; Apple Home plus Home Assistant has not been
hardware-validated on this sibling target, so the Zephyr application remains
the current product and hardware oracle.

The binding constraints remain the 512 KB flash and 128 KB RAM budgets. The
current Zephyr production oracle no longer overflows RAM: it uses 120,740 B and
leaves 10,332 B. Measure the FreeRTOS Matter ELF independently rather than
transferring those numbers between operating systems.

The selected board source base is Qorvo DW3/QM33 SDK v1.1.1;
this port owns the pinned OpenThread MTD, nRF52833 802.15.4
integration, and credential/Matter L2CAP CoC backend layered onto it. The radio
runtime is paired with the pinned MPSL/SoftDevice Controller, nRF 802.15.4,
and Apache NimBLE host source set.

The parity scope remains the shipping Matter-over-Thread reader and the UWB
self-test. The Zephyr and FreeRTOS media formats are intentionally not
interchangeable, and the v0.3 Matter/Home Key identity is a clean-break upgrade
on both.
