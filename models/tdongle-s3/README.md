# LilyGO T-Dongle-S3

A USB stick with an ESP32-S3, a 160x80 ST7735 and a microSD slot. Its job in
the fleet is to be the **BLE5 bridge**: an S3, so it has extended advertising
and can carry an XPRS packet on the air (`docs/esp32.md`, "Radio capability
per chip"), and a stick, so it lives plugged into something beside a router.

What it hears on Bluetooth it repeats on the LAN and on ESP-NOW; what it
hears on the LAN it repeats on Bluetooth; and on each of those it
**digipeats** -- repeating what it heard on the medium it heard it on,
appending itself to `via:` within the hop budget (XPRS 13.1). None of that is
written in this board's firmware: every bearer does it, and `xprs_app` wires
them together the same way on every board.

## What is in here

| | |
|---|---|
| Chip | `esp32s3`, no PSRAM |
| `firmware/` | the build that ships |
| `sdkconfig.tdongle_s3` | this board's ESP-IDF configuration, read by the shared `multiboard` build |
| `docs/` | anything true of this board and not of the others |
| `hardware/` | pinouts, photos, enclosures, 3D prints -- empty until there are some |

## Building it

```sh
cd firmware
~/.platformio/penv/bin/pio run
~/.platformio/penv/bin/pio run -t upload
```

The upload port is pinned by-id (`48:CA:43:4B:B7:C4`), so with a T-Deck on
the same bench `-t upload` cannot reach the wrong board.

It takes the shared libraries by symlink from `common/`, so a fix there
reaches every board that uses it without copying.

The older `tdongle_s3` target in `../../multiboard` still exists and is the
legacy BLE APRS build. It is not what ships here.

## The station is not in this project

`firmware/src/main.c` is about 190 lines and contains no station logic: it is
the ST7735 on the pins in `common/xprs_model_tdongle_s3`, the BOOT strap pin
(the only button there is), and one `xapp_board_t` handed to `xapp_run()`.
Everything else -- the bearers, the indexer, the HTTP API, the updater, the
signing, the gossip -- is `common/xprs_app`, shared with the T-Deck and the
M5Stack.

It used to be five thousand lines and its own station, with an APRS-IS iGate,
a BLE street mesh with a GATT service and a Reticulum TCP uplink beside it.
Those are gone; `git log` has them, and the place for them is a shared
component rather than one board's `main.c`.

### The screen

160x80, which is smaller than the shared dashboard will draw: `xui_init()` in
`xprs_ui` refuses anything under 160x120 rather than render something
illegible. So this board links the OTHER implementation of the same
interface, `xprs_ui_mini`, which folds the seven panels into three views --
DEVICES, STATS, CHAT -- and rotates them hands-off.

The tour matters here. The board's only control is the BOOT strap pin, GPIO0,
half under the case in most mountings, so the screen has to be worth looking
at without being touched. Three gestures when it is reachable:

| | |
|---|---|
| tap | next panel. The first tap also stops the tour, so a tap is how you hold a view still to read it. |
| hold ~0.7 s | back to the home view. |
| keep holding to ~2 s | home, then the tour starts again. |

### What this board runs at

No PSRAM, so internal DRAM is the whole budget and the order things start in
is the allocator (`docs/esp32.md`). Measured on this firmware, associated,
BLE up, archive mounted:

```
heap after ble    118,792 free (largest 57,344)
heap after wifi    58,980 free (largest 31,744)
heap after api     45,784 free (largest 31,744)
steady             ~21,000 free, worst case ~1,000
```

Two settings are what make that fit, and both are decided by this board
rather than by the operator's taste:

- **No walk-up hotspot** (`.hotspot = false`). The access point costs about
  9 KB: with it up the station ran at 12 KB free and touched **96 bytes** at
  its worst moment. A board beside a router does not need to be one. It is a
  default, not a verdict -- `cfg set ap_on 1` over the cable turns it on.
- **`CONFIG_SDCARD_MAX_FILES=5`**, not the 3 that was right when the archive
  lived on the card. The station holds five things open on the internal FAT
  at once; at three it printed `vfs_fat: open: no free file descriptors`
  every couple of seconds and the sixth thing to ask was simply not written.

### The archive moved into flash

`partitions.csv` now carries a wear-levelled FAT `storage` partition at the
same offset and size as the T-Deck's, and the indexer, the log, the stats and
the chat all live there. Before this the archive was on the microSD card, so
a dongle with an empty slot kept nothing, and the log had a raw partition
reserved for it that was never written.

The first boot on the new table formats that partition once. **NVS is
untouched at `0x9000`**, so the station keeps its identity, its callsign and
its WiFi credentials across the change.
