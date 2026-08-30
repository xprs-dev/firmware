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
config, its documentation, its photos, pinouts and 3D-printed cases. Three
boards carry their own firmware project there as well; the rest are targets
of the shared `multiboard/` build.

Each board folder also holds a `board.yml` -- the same facts as its README
but flat, so a program can read them. That is what a download-and-browse
catalogue page would be built on; [`docs/catalog.md`](docs/catalog.md) is
the schema and `models/_template/` is the empty shape to copy. The files are
checked for shape by `tools/scripts/check_board_yml.py`, and rendered by
`tools/scripts/build_catalog.py` into [`index.html`](index.html)
and [`docs/boards.json`](docs/boards.json) -- every board as JSON, with a
link to its folder here, its vendor page and manual, what its radios are,
and which XPRS roles (digipeater, iGate, hotspot, indexer, ...) it fills.
[`docs/board.template.json`](docs/board.template.json) is the empty shape.
`tools/scripts/collect_prebuilt.py` copies each built image into
`models/<board>/prebuilt/` with a web-flasher manifest, and the page offers
them for download or, served over HTTPS in Chrome, flashes them directly.


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
| [`sensecap-p1-pro`](models/sensecap-p1-pro/) | **nRF52840** | none yet -- solar outdoor node; LoRa + BLE5, no WiFi, not an ESP32 |

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

The last row is not an ESP32 at all. The SenseCAP P1-Pro is an nRF52840,
so nothing in `common/` compiles for it and it has no firmware here yet; it
is catalogued in `models/` because that is where the fleet's boards live,
and its README says plainly what a port would take.

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
`xprss/xprs-esp32`, and it was called `xprs-esp32` here too until the SenseCAP
P1-Pro arrived and made the name untrue: it is an nRF52840, and the tree now
builds firmware for two chip families rather than one.

Three strings still say `xprs-esp32` and are deliberately left alone, because
they are not the repository's name -- they are this software's name ON THE
AIR and over HTTP: the `app` field of `/api/status` (`xprs_app.c`), the
`service` field of the BLE hello payload (`xprs_ble.c`), and the comment
documenting the first. Changing those changes what every station reports to
clients that already parse it, which is a protocol decision and not a rename.
