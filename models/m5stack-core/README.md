# M5Stack Core (ESP32)

The second voice on the air. An original ESP32 (D0WDQ6), so it has **no** BLE5
extended advertising and can never join the Bluetooth plane -- which is the
point: it proves a bearer works between two stations rather than within one.
It speaks XPRS over ESP-NOW and the LAN, signs and verifies.

## What is in here

| | |
|---|---|
| Chip | `esp32` |
| `firmware/` | its own PlatformIO project |
| `docs/` | anything true of this board and not of the others |
| `hardware/` | pinouts, photos, enclosures, 3D prints -- empty until there are some |

## Building it

This board has its own PlatformIO project, because it is not one of the shared project's targets at all -- it was added
later, for the bearers rather than for the older APRS firmware.

```sh
cd firmware
~/.platformio/penv/bin/pio run
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/ttyUSB0
```

It takes the shared libraries by symlink from `common/`, so a fix there reaches
every board that uses it without copying.

A CP2104 on `/dev/ttyUSB0`. Opening that port asserts DTR and reboots the
board, so open it once and leave it open when measuring.

