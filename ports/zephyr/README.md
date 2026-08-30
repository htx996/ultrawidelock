# Zephyr port

`zephyr/module.yml` exposes this directory as one Zephyr module; `CMakeLists.txt`
and `Kconfig` select the backend sources. Applications add it through
`ZEPHYR_EXTRA_MODULES`.

- `osal/`, `log/`, `store/` implement system contracts.
- `ble/`, `matter/`, `nfc/` connect protocol transports.
- `dw3000/`, `uwb/`, `drivers/` connect hardware.
- `dfu/`, `shell/` connect Zephyr subsystems to portable features.

Reusable protocol logic belongs in `modules/`.
