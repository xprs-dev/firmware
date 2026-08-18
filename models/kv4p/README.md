# kv4p HT

A VHF handheld radio interface: an SA818 module on an ESP32, so this board can
put packet on the air rather than only on WiFi and Bluetooth.

## What is in here

| | |
|---|---|
| Chip | `esp32` |
| `sdkconfig.kv4p` | this board's ESP-IDF configuration, read by the shared build |
| `docs/` | anything true of this board and not of the others |
| `hardware/` | pinouts, photos, enclosures, 3D prints -- empty until there are some |

## Building it

The shared multi-target project builds this board:

```sh
cd ../../multiboard
~/.platformio/penv/bin/pio run -e kv4p
```

One `src/main.cpp` serves all eight targets in that project; what makes this
board itself is `sdkconfig.kv4p` here and the `geogram_model_*` component it
selects from `common/`.

