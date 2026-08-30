# Heltec WiFi LoRa 32 (v2)

LoRa board, SX1276 radio, OLED.

## What is in here

| | |
|---|---|
| Chip | `esp32` |
| `board.yml` | the catalogue entry, machine-readable (`docs/catalog.md`) |
| `sdkconfig.heltec_v2` | this board's ESP-IDF configuration, read by the shared build |
| `docs/` | anything true of this board and not of the others |
| `hardware/` | pinouts, photos, enclosures, 3D prints -- empty until there are some |

## Building it

The shared multi-target project builds this board:

```sh
cd ../../multiboard
~/.platformio/penv/bin/pio run -e heltec_v2
```

One `src/main.cpp` serves all eight targets in that project; what makes this
board itself is `sdkconfig.heltec_v2` here and the `xprs_model_*` component it
selects from `common/`.

