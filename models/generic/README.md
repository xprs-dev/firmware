# Any ESP32 board (esp32dev)

The board for trying XPRS without buying anything: any board built on an
**original ESP32 with 4 MB of flash or more**. That is the Espressif
ESP32-DevKitC, the DOIT DevKit V1, and the many WROOM-32 boards sold under
other names. It runs `common/xprs_app`, the same station as every other
board, on **ESP-NOW and the LAN**, with the HTTP API and the walk-up hotspot
whose chat page is how a person uses a board with no screen.

The image drives no pin and reads none, so it is safe on whatever a
particular board has wired up. Not for the ESP32-S3 or C3: those are other
chips with their own images (`esp32c3-mini` runs on any C3 board).

## What is in here

| | |
|---|---|
| `firmware/` | this board's PlatformIO project |
| `board.yml` | the catalogue entry, machine-readable (`docs/catalog.md`) |
| `hardware/` | the DevKitC's overview and pinout, from Espressif |
| `sdkconfig.esp32_generic` | the OLD multiboard build's config, not used by `firmware/` |

## Trying it

1. **Flash it.** On [xprs.dev/firmware](https://xprs.dev/firmware/#generic),
   in Chrome or Edge on a desktop, plug the board in and press Install. If the
   browser cannot connect, hold the board's BOOT button while it does. Or
   download the three files and write them with esptool: the bootloader at
   `0x1000`, the partition table at `0x8000`, the firmware at `0x20000`.
2. **Use it from a phone.** With no network set, the board opens its own
   access point, `XPRS-<callsign>`. Join it and the chat page comes up.
3. **Put it on your network** (optional, and what lets it bridge to the
   others): open a serial terminal on the board's port at 115200 baud (the
   Arduino IDE's Serial Monitor, PuTTY, `screen`, or `pio device monitor`),
   type these two lines, and press the EN (reset) button:

   ```
   cfg set ssid <your network>
   cfg set pass <its password>
   ```

   They live in NVS, which a reflash keeps unless it erases the flash. The
   callsign is in the boot log, and once the board is on the network,
   `http://<its address>/` is the chat page and `/api/status` says what it
   is doing.

Two XPRS boards hear each other over ESP-NOW with no network at all, as long
as they are on the same WiFi channel: both on the same access point, or both
with none set (they then use channel 1).

## Building and flashing

```sh
cd firmware
~/.pio-venv/bin/pio run            # build
~/.pio-venv/bin/pio run -t upload  # flash
```

Copy `src/wifi_secrets.h.example` and `src/fw_secrets.h.example` to the
names without `.example` first; both are gitignored. Anything in
`wifi_secrets.h` is compiled into the image, so the published one is built
from the empty example. No port is pinned; add `upload_port` to
`platformio.ini` when more than one board is plugged in.

**Coming from other firmware**: an Arduino sketch leaves its own partition
table, whose `otadata` overlaps this one's NVS and `otadata`. The web
installer offers to erase on a first install; say yes. With esptool, erase
once (`esptool.py erase_flash`) before the first flash. Erasing also clears
any identity the board had.

**Changing `sdkconfig.defaults`**: delete `firmware/sdkconfig.generic`
before building. A default only fills keys the generated file does not
already have, so a new line is silently ignored otherwise.

## What it cannot do, and why

**No BLE.** The original ESP32 has Bluetooth 4.2, whose adverts carry 31
bytes; an XPRS beacon is 112 to 173. Bluetooth stays off, which also leaves
its RAM and airtime to WiFi. The M5Stack Core, the same chip, runs the same
way.

**No archive.** Two app slots on 4 MB leave an 832 KB volume for the log,
the statistics and the conversation. One index segment is 1.3 MB and the
one being written can never be evicted, so there is no archive and nothing
to share from one (`storage_mount` in `firmware/src/main.c`). A board with
more flash still gets the 4 MB layout, so one image fits them all.

## Where the settings come from

The M5Stack Core is the same chip running the same station with a screen,
so `sdkconfig.defaults` carries its proven settings: the capped WiFi and TCP
buffers that let a firmware push finish on a board with no PSRAM, the
signed plain-HTTP update path, rollback and the 90 s watchdog. The shape is
the C3's: no display (`display_init = NULL`, `common/xprs_ui_none`), the
serial console kept, and no archive.

## Built, 2026-09-11

| | |
|---|---|
| image | 1,267,568 bytes, 80.6% of a 1.5 MB slot |
| static RAM (`.data` + `.bss`) | 104,584 bytes |

Built, not yet run: no bare ESP32 devkit is on the bench. The same station
has run on the M5Stack Core, which has the same chip.
