# Heltec WiFi LoRa 32 (v3)

LoRa board on an ESP32-S3, SX1262 radio, OLED.

## What is in here

| | |
|---|---|
| Chip | `esp32s3` |
| `sdkconfig.heltec_v3` | this board's ESP-IDF configuration, read by the shared build |
| `docs/` | anything true of this board and not of the others |
| `hardware/` | pinouts, photos, enclosures, 3D prints -- empty until there are some |

## Building it

The shared multi-target project builds this board:

```sh
cd ../../multiboard
~/.platformio/penv/bin/pio run -e heltec_v3
```

One `src/main.cpp` serves all eight targets in that project; what makes this
board itself is `sdkconfig.heltec_v3` here and the `xprs_model_*` component it
selects from `common/`.

