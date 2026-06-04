# Xiaomi Pen Status

Small Qt tray utility for stylus power-state attributes exported by
`qcom_battmgr`. The UI follows the system locale and supports Chinese and
English. Set `XIAOMI_PEN_LANG=zh` or `XIAOMI_PEN_LANG=en` to override it.

Build:

```sh
qmake6
make
```

Run:

```sh
./xiaomi-pen-status
```

Closing the window keeps the tray icon running. Use the tray menu to show the
window again or quit.

The default sysfs path is:

```text
/sys/devices/platform/pmic-glink/pmic_glink.power-supply.0/xiaomi
```

Override it with `XIAOMI_PEN_SYSFS` when testing.
