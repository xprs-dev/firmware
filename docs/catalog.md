# The board catalog

Every board lives in `models/<id>/`, and this page is what that folder is
supposed to contain and why. It exists because the same facts were being
asked for in four places -- a README written for whoever is holding the
board, a `platformio.ini` comment, a commit message, and increasingly a
person on a web page choosing which `.bin` to download -- and prose cannot
be read by the last of those.

So there are two artefacts and they have different jobs:

| | |
|---|---|
| `README.md` | for somebody working ON the board. Prose, opinionated, says why. |
| `board.yml` | for a program. One board's facts, flat, no prose, no marketing. |

`board.yml` is the one a catalogue page reads. `tools/scripts/build_catalog.py`
renders all of them into `index.html` at the repository root (so its
`models/<id>/...` image paths resolve) and, with `--json`, into
`docs/boards.json` for a site that wants the facts rather than the page.

## The folder

```
models/<id>/
  board.yml            required -- the schema below
  README.md            required -- prose, for whoever works on it
  hardware/
    HARDWARE.md        pinout, block diagram, what is wired to what
    images/            photographs and diagrams; see "Images" below
  docs/                anything true of this board and of no other
  firmware/            its own PlatformIO project, IF it has one
  sdkconfig.<target>   its ESP-IDF config, IF it is a multiboard target
  prebuilt/            what tools/scripts/collect_prebuilt.py copied out of the
                       last `pio run`: bootloader, partitions, firmware and an
                       ESP Web Tools manifest.json (ESP boards) or firmware.uf2
                       (nRF52). The page's Install button and download links
                       come from here.
```

`models/_template/` is a copy of this with every field present and empty.
Start from it.

## board.yml

Every key below is required unless it says otherwise. **A field nobody has
measured is `null`, never a guess** -- a catalogue that invents a weight is
worse than one with a blank in it, because a blank can be filled and a
plausible lie cannot be found.

```yaml
id:            models/ folder name. kebab-case. the primary key.
name:          what the vendor calls it
vendor:        who makes it
sku:           vendor part number, or null
product_url:   where to buy it, or null
manual_url:    the vendor's manual, datasheet or wiki for THIS board, or null
tagline:       one line, for the gallery tile at the top of the page: what
               the device is and what it carries. Under 120 characters.
summary:       a paragraph. What the device is -- MCU, memory, screen, radios,
               ports -- then what the station on it does and what it is used
               for here. This is the card text; a reader identifies the board
               from it.
status:        shipping | legacy | planned | unsupported
               planned    = we have the hardware, no firmware yet
               legacy     = firmware exists but a newer board supersedes it
               unsupported= documented so nobody tries it again

silicon:
  mcu:         part number, e.g. ESP32-S3 / nRF52840
  family:      the TOOLCHAIN family, not the marketing name:
               esp32 | esp32s3 | esp32c3 | nrf52
               This is what says whether common/ can be reused at all.
  core:        e.g. "Xtensa LX7 dual @ 240 MHz"
  ram_kb:      internal RAM
  psram_mb:    null when there is none. A number here changes what fits.
  flash_mb:    program flash

radios:        what the silicon and the modules physically carry, in the
               vendor's terms. bluetooth {version, extended_advertising, note},
               wifi {standards, band_ghz, modes} or null, lora {chip, band_mhz}
               or null, other [] for anything else that transmits (an SA818).
               This answers "what does the board HAVE"; bearers and xprs below
               answer "what does it DO", and the three are kept apart because
               a chip that can is not a firmware that does.

bearers:       the XPRS-specific section, and the reason this file exists.
               One key per bearer in common/: ble5, lora, lan, espnow.
               Each is `yes`, `no` or `untested`, with a `_note` beside it
               giving the REASON -- "no BLE5 on this chip", "no WiFi radio".
               A catalogue that lists radios tells you what the board has;
               this tells you what it can do on the network, which is the
               question somebody downloading firmware is actually asking.

xprs:          what the station on this board does on the network, one key
               per role, each `yes`, `no`, `planned` or `untested`, with a
               `_note` for anything that is not a plain yes. The roles are the
               ones /api/status reports plus the ones a person asks about:
                 beacon        says who it is, on every bearer it has
                 digipeater    repeats what it heard on the medium it heard it
                 bridge        carries a packet from one bearer onto another
                 igate         APRS-IS uplink over IP
                 hotspot       its own access point with the chat page on it
                 api           the HTTP API (/api/status, /api/xprs/...)
                 indexer       the card-backed XPRS index
                 share         serves its archive to other stations
                 reticulum     a Reticulum bearer or hub
                 ota           signed over-the-air updates
                 gossip        the peer table exchange
                 dashboard     a screen with the station's panels on it
                 chat          can be TYPED on: the interactive chat panel
                 mesh_session  the 1:1 channel over a BLE connection
                 vhf           packet on a VHF handheld through an SA818

io:            screen (or null), buttons, leds, gnss, connectors, sensors
physical:      dimensions_mm, weight_g, ip_rating, temp_c, power
firmware:      toolchain, project, env, version, artifact, flashing,
               flash_port: usb-serial | native-usb | uf2 -- how bytes reach
               it, which is not the same as the chip family (the Heltec V3
               is an S3 behind a CP2102). The page picks its flashing
               section from this.
docs:          list of {title, url}
images:        list of {file, caption, credit} -- photographs of the HARDWARE
screenshots:   list of {file, caption} -- what the FIRMWARE shows: the screen,
               the hotspot page, the dashboard. Empty on a headless board.
```

## board.json

`tools/scripts/build_catalog.py --json docs/boards.json` writes every
board as one JSON array, the same facts with three things added that a
program outside this tree cannot derive on its own: `source_url` (the
board's folder on GitHub, where a change is made), `image_url` on every
image and screenshot (absolute, raw.githubusercontent.com), and `repo`.
`docs/board.template.json` is the empty shape, generated from
`models/_template/board.yml` the same way. Neither is edited by hand.

## Images

Paths are relative to the board folder, so a page can join them without
knowing this tree's layout.

**Every image carries a `credit`.** Vendor product photography is the
vendor's, not ours, and a catalogue that publishes it is redistributing it
-- which is ordinary and expected for a product listing, and is exactly why
the source has to travel with the file rather than be remembered. A photo
taken on this bench says so too, for the opposite reason: it is the one
somebody can ask to have retaken.

Prefer, in this order: a labelled block diagram or pinout from the vendor's
datasheet; a photograph of the actual board on this bench; vendor product
photography. The first is the only one that answers a question while
soldering.
