# ESP32-S3 with a 1.54" e-paper display

An e-paper station. The display driver is `geogram_epaper_1in54` in `common/`.

## What is in here

| | |
|---|---|
| Chip | `esp32s3` |
| `sdkconfig.esp32s3_epaper_1in54` | this board's ESP-IDF configuration, read by the shared build |
| `docs/` | anything true of this board and not of the others |
| `hardware/` | pinouts, photos, enclosures, 3D prints -- empty until there are some |

## Building it

The shared multi-target project builds this board:

```sh
cd ../../multiboard
~/.platformio/penv/bin/pio run -e esp32s3_epaper_1in54
```

One `src/main.cpp` serves all eight targets in that project; what makes this
board itself is `sdkconfig.esp32s3_epaper_1in54` here and the `geogram_model_*` component it
selects from `common/`.

