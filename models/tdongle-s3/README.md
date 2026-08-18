# LilyGO T-Dongle-S3

The reference station: a USB dongle with an ESP32-S3, an ST7735 screen and a
microSD slot. It carries BLE5 extended advertising, WiFi, the XPRS LAN and
ESP-NOW bearers, an APRS-IS iGate, a Reticulum hub and the card-backed XPRS
index -- all through one radio, which is why `docs/esp32.md` spends its length
on heap and on the two processors.

## What is in here

| | |
|---|---|
| Chip | `esp32s3` |
| `firmware/` | the build that actually ships (was `rns_ble5`) |
| `sdkconfig.tdongle_s3` | this board's ESP-IDF configuration, read by the shared build |
| `docs/` | anything true of this board and not of the others |
| `hardware/` | pinouts, photos, enclosures, 3D prints -- empty until there are some |

## Building it

This board has its own PlatformIO project, because it outgrew the shared one: BLE5 extended advertising, the SD-backed
index and the Reticulum hub do not belong in a build shared with boards
that have none of them.

```sh
cd firmware
~/.platformio/penv/bin/pio run -e rns_ble5
~/.platformio/penv/bin/pio run -e rns_ble5 -t upload --upload-port /dev/ttyACM0
```

It takes the shared libraries by symlink from `common/`, so a fix there reaches
every board that uses it without copying.

The older `tdongle_s3` target in `../../multiboard` still exists and is the
legacy BLE APRS build. It is not what ships here.

