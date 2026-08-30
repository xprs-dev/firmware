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

`board.yml` is the one a catalogue page reads. Nothing in this tree parses it
yet; it is written first so that the page, when it exists, is not a rewrite
of ten READMEs by hand.

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
  prebuilt/            a built artefact, when one is published
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
summary:       one line. what this board is FOR in the fleet, not a spec dump.
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

bearers:       the XPRS-specific section, and the reason this file exists.
               One key per bearer in common/: ble5, lora, lan, espnow.
               Each is `yes`, `no` or `untested`, with a `_note` beside it
               giving the REASON -- "no BLE5 on this chip", "no WiFi radio".
               A catalogue that lists radios tells you what the board has;
               this tells you what it can do on the network, which is the
               question somebody downloading firmware is actually asking.

io:            screen (or null), buttons, leds, gnss, connectors, sensors
physical:      dimensions_mm, weight_g, ip_rating, temp_c, power
firmware:      toolchain, project, env, version, artifact, flashing
docs:          list of {title, url}
images:        list of {file, caption, credit}
```

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
