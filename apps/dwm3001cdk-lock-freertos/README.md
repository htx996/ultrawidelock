# DWM3001CDK Lock, FreeRTOS

The Zephyr-free port of `apps/dwm3001cdk-lock`. FreeRTOS OSAL, radio, Thread,
credential, UWB and the optional portable Matter graph live in
`ports/freertos-nrf52833`. Host gate:

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

The Matter variant shares the portable commissioning, five-fabric, Thread
staging, selective rollback/removal, SRP and `mf2` record code with the Zephyr
image, over a settings backend with the same transaction semantics on a different
on-flash log format. Behaviour and power-cut cases are host-tested. The Zephyr
application remains the hardware-validated product and the behavioural oracle.

512 KB flash and 128 KB RAM still bind. The Zephyr oracle uses 120,740 B of RAM
and leaves 10,332 B. Measure the FreeRTOS Matter ELF independently rather than
carrying those numbers between operating systems.

Board source base: Qorvo DW3/QM33 SDK v1.1.1. This port owns the pinned
OpenThread MTD, the nRF52833 802.15.4 integration and the credential/Matter
L2CAP CoC backend layered onto it, paired with the pinned MPSL/SoftDevice
Controller, nRF 802.15.4 and Apache NimBLE host source set.

Parity scope is the shipping Matter-over-Thread reader and the UWB self-test.
The Zephyr and FreeRTOS media formats are deliberately not interchangeable, and
the v0.3 Matter/Home Key identity is a clean-break upgrade on both.
