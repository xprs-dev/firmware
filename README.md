# XPRS on ESP32

Firmware for the ESP32 boards that speak [XPRS](https://github.com/xprs-dev/spec).

A station here is a small radio that carries XPRS over whatever it has: BLE5
extended advertising, ESP-NOW, WiFi on the LAN, LoRa, or a VHF handheld through
an SA818. Some of these boards also run an APRS-IS iGate, a Reticulum hub and a
card-backed XPRS index -- one radio, many jobs, which is why the memory and
scheduling notes in [`docs/esp32.md`](docs/esp32.md) are the first thing to read
before touching any of it.

## Layout

```
common/       the shared component library -- every board draws from here
models/       one folder per board (below)
multiboard/   the PlatformIO project that builds eight of the older boards
tools/        espnow_probe, flashing and monitoring scripts, host tests
docs/         what is true across boards
```

**`models/<board>/` is where anything board-specific belongs**: its ESP-IDF
config, its documentation, its photos, pinouts and 3D-printed cases. Two boards
carry their own firmware project there as well; the rest are targets of the
shared `multiboard/` build.

| Board | Chip | Firmware |
|---|---|---|
| [`tdongle-s3`](models/tdongle-s3/) | ESP32-S3 | own project -- the BLE5 bridge, 160x80 |
| [`m5stack-core`](models/m5stack-core/) | ESP32 | own project -- the second voice on the air |
| [`tdeck`](models/tdeck/) | ESP32-S3 | own project -- screen, trackball, keyboard; 868 MHz radio idle |
| [`heltec-v1`](models/heltec-v1/) [`v2`](models/heltec-v2/) [`v3`](models/heltec-v3/) | ESP32 / S3 | `multiboard` -- LoRa, SX1276 / SX1262 |
| [`kv4p`](models/kv4p/) | ESP32 | `multiboard` -- VHF handheld via SA818 |
| [`esp32c3-mini`](models/esp32c3-mini/) | ESP32-C3 | `multiboard` |
| [`epaper-1in54`](models/epaper-1in54/) | ESP32-S3 | `multiboard` -- e-paper display |
| [`generic`](models/generic/) | ESP32 | `multiboard` -- plain devkit |

## Building

The BLE5 bridge:

```sh
cd models/tdongle-s3/firmware
~/.platformio/penv/bin/pio run
~/.platformio/penv/bin/pio run -t upload
```

The port is pinned by-id in each project's `platformio.ini`, so `-t upload`
cannot reach the wrong board when two are plugged in.

One of the shared targets:

```sh
cd multiboard
~/.platformio/penv/bin/pio run -e heltec_v3
```

Each board's own README says which of the two applies and why.

## Shared code

`common/` holds the component library, `xprs_*` by prefix. The XPRS pieces
are `xprs_codec` (the codec), `xprs_bearer` (the queue, the duplicate
rings, the relay decision, shared by every bearer), `xprs_bearer_lan`,
`xprs_bearer_now`, `xprs_chan` (meeting on a working channel, spec
section 23.7), `xprs_id`, `xprs_sig` and `xprs_index`.

**The station itself is `xprs_app`**, and the three boards with their own
project all run it: the T-Deck, the M5Stack and the T-Dongle. A board's
`main.c` is its screen, its pins and how a person presses something,
described in one `xapp_board_t` and handed to `xapp_run()` -- 150 to 650
lines, no station logic at all. A fix to a bearer, a panel or the indexer is
made once and every board has it.

The screen is the one thing the boards genuinely disagree about, so it is
an interface with two implementations: `xprs_ui_api` is the header,
`xprs_ui` draws the seven-panel dashboard on 240px-and-up, and
`xprs_ui_mini` folds those panels into three views on the T-Dongle's 160x80
strip. A board names exactly one of the two in its `REQUIRES`.

Projects take them by symlink rather than by copy, so a fix reaches every board
that uses it. Several components carry **host test suites** that build with gcc
and need no hardware:

```sh
cd common/xprs_sig && ./test_xprssig_host.sh
```

Run those before flashing anything. A state machine is the wrong thing to debug
over a radio link.

## Testing on hardware

`docs/esp32.md` is binding on how measurements are taken here -- reachability
reported as *n* of *m*, no serial port opened while measuring, heap read before
believing anything. Claims about this firmware that are not backed that way have
been wrong often enough to be worth the rule.

`tools/espnow_probe/` is a deliberately empty firmware -- ESP-NOW and nothing
else -- for answering questions the full station cannot, because on it nine
subsystems share one radio and none of them can be removed. It is what found
that a running BLE controller costs an unassociated station every incoming
frame; see [`docs/espnow.md`](docs/espnow.md).

## History

This tree was developed inside the Aurora application repository as `aurora/esp32`
and moved here when the project took the XPRS name. Its earlier history is in
`xprss/xprs-esp32`.
