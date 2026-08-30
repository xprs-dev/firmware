# Generic ESP32 (esp32dev)

The plain devkit target: no screen, no radio module, nothing board-specific.

## What is in here

| | |
|---|---|
| Chip | `esp32` |
| `board.yml` | the catalogue entry, machine-readable (`docs/catalog.md`) |
| `sdkconfig.esp32_generic` | this board's ESP-IDF configuration, read by the shared build |
| `docs/` | anything true of this board and not of the others |
| `hardware/` | pinouts, photos, enclosures, 3D prints -- empty until there are some |

## Building it

The shared multi-target project builds this board:

```sh
cd ../../multiboard
~/.platformio/penv/bin/pio run -e esp32_generic
```

One `src/main.cpp` serves all eight targets in that project; what makes this
board itself is `sdkconfig.esp32_generic` here and the `xprs_model_*` component it
selects from `common/`.

