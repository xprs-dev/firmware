# ESP32-C3 Mini

A small RISC-V board. Built by the shared multi-target project.

## What is in here

| | |
|---|---|
| Chip | `esp32c3` |
| `sdkconfig.esp32c3_mini` | this board's ESP-IDF configuration, read by the shared build |
| `docs/` | anything true of this board and not of the others |
| `hardware/` | pinouts, photos, enclosures, 3D prints -- empty until there are some |

## Building it

The shared multi-target project builds this board:

```sh
cd ../../multiboard
~/.platformio/penv/bin/pio run -e esp32c3_mini
```

One `src/main.cpp` serves all eight targets in that project; what makes this
board itself is `sdkconfig.esp32c3_mini` here and the `geogram_model_*` component it
selects from `common/`.

