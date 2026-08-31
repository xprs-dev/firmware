# Heltec WiFi LoRa 32 V3

An ESP32-S3 with an SX1262 on 868 MHz and a 0.96" 128x64 OLED, the size of
a matchbox, with one button. Its job in the fleet is to be the **shelf
digipeater**: it carries the T-Deck's whole radio set -- LoRa, BLE5
extended advertising, WiFi for the LAN and ESP-NOW -- without the keyboard,
the touch panel or the price, so it is the board you leave on a windowsill
to relay what the others say. It runs `common/xprs_app`, the same station
as the T-Deck, the M5Stack and the T-Dongle.

| | |
|---|---|
| Chip | `esp32s3`, 512 KB RAM, no PSRAM, 8 MB flash |
| Radios | SX1262 LoRa (868 MHz here; 433/915 sold), BLE5, WiFi 2.4 GHz |
| Screen | SSD1306 128x64 monochrome, I2C |
| Button | PRG on GPIO0. RST is a reset line, not an input. |
| USB | CP2102 USB-serial -- `/dev/ttyUSB*`, not the S3's native port |
| `board.yml` | the catalogue entry, machine-readable (`docs/catalog.md`) |
| `firmware/` | the build that ships: PlatformIO, ESP-IDF, env `heltec_v3` |
| `sdkconfig.heltec_v3` | the OLD fragment for the shared `multiboard` build, which does not compile for this board and is not what ships |
| `prebuilt/` | bootloader, partitions, firmware and the web-flasher manifest |

## Building it

```sh
cd firmware
~/.platformio/penv/bin/pio run
~/.platformio/penv/bin/pio run -t upload
```

The upload port is pinned by-id to the CP2102, so with a T-Deck and a
T-Dongle on the same bench `-t upload` cannot reach the wrong board. The
CP2102 means esptool's stub and 921600 baud both work; none of the
T-Dongle's `--no-stub` / 115200 caveats apply here. Opening the port does
**not** reset the board (uptime carries across a `monitor-capture.sh`), so
its serial log is a usable counter during a measurement in a way the
T-Dongle's is not -- but `monitor-capture.sh -r` leaves it parked in the
ROM bootloader; use `esptool ... --after hard_reset read_mac` to get it back.

## The station is not in this project

`firmware/src/main.c` is about 230 lines and contains no station logic: the
SSD1306 on the pins in `common/xprs_model_heltec_v3`, the SX1262 handed to
the LoRa bearer as a pin table, the battery divider, the PRG button, and one
`xapp_board_t` handed to `xapp_run()`. Everything else -- the four bearers,
the digipeater, the indexer, the HTTP API, the updater, the gossip -- is
`common/xprs_app`.

Two things in the tree changed to make room for it:

- **`common/xprs_model_heltec_v3` no longer brings up the SX1262.**
  `xprs_bearer_lora` creates the radio itself from the pins in `main.c`,
  exactly as on the T-Deck; a second driver on the same SPI bus and CS pin
  was two owners of one radio. The battery read moved from the deprecated
  `driver/adc.h` to `esp_adc` oneshot with curve-fitting calibration.
- **`xprs_ssd1306` and `xprs_model_heltec_v3` declare their requirements
  unconditionally.** ESP-IDF resolves `REQUIRES` before Kconfig is loaded,
  so a `REQUIRES` behind `if(CONFIG_XPRS_BOARD_HELTEC_V3)` is silently
  dropped and nothing can find `ssd1306.h`. Only the source list is gated
  now, the pattern `xprs_st7735` already carried and explained.

### The screen

`xprs_ui_mini` -- the T-Dongle's three-view UI -- draws RGB565 through LVGL;
the SSD1306 holds one bit per pixel. The bridge is the `display_flush` in
`main.c`: each pixel's luminance (0.30/0.59/0.11, in integers) against a
cut of 80 out of 255, set into the panel's page buffer with
`ssd1306_draw_pixel`, and `ssd1306_display()` once when the strip that ends
on row 63 arrives. No dithering: at 10 px type on 64 rows there is nothing a
dither would improve and everything it would smear. The cut is low on
purpose, so the UI's dim greys and dark oranges are still drawn rather than
vanishing.

128x64 is exactly the mini UI's floor (`width >= 120, height >= 64`). It
holds four list rows under the 13 px top bar, not the T-Dongle's five, and
that is now the UI's own arithmetic (`rows_fit()` in `xprs_ui_mini.c`)
rather than a constant: 160x80 still gets five.

### The button

PRG is the strap pin, active low. The T-Dongle's grammar, unchanged: tap =
next view (and the first tap stops the tour), hold 0.7 s = home, keep holding
to 2 s = restart the tour. The board asks for the tour at boot
(`.rotate = true`) for the same reason the T-Dongle does: one button.

### What this board runs at

No PSRAM, so internal heap is the whole of its room, and this is the
T-Dongle's config with two changes and the LoRa bearer added:

- `CONFIG_XPRS_BEARER_RNS=n`. The hub link's one TCP socket and task cost
  the T-Dongle 12,676 bytes; this board finished its first boot with 6,440
  free and a min-ever of 16, and it does not dial a hub.
- `CONFIG_BT_CTRL_BLE_MAX_ACT=2` (the T-Deck's value) and the LVGL pool at
  12 KB, since 128x64 is 64% of the T-Dongle's pixels.

Measured 2026-08-30: end of boot 6,440 free with the bearers just started
on a bench of four stations digipeating one another; on a quieter bench
14-15 KB steady with a min-ever of 9.4 KB. Under the storm it touched 172
bytes and lost signature checks to `OUT OF MEMORY`. `.heap_floor = 4000`,
under the transient and over the failure.

## The first evening on the air, and what it broke

The Heltec came up on its first boot as X3H3MZ: BLE5 up in 1.2 s, the
OLED and the mini UI at 1.6 s, `xprslora: up: 868000000 Hz SF7/125k 14 dBm`
at 2.4 s, WiFi associated at 2.6 s, and hearing and digipeating the
T-Deck's beacons on BLE before the splash was gone.

That made it the fourth station on a bench where every one digipeats the
others on BLE, and the T-Deck -- with PSRAM, the fleet's roomiest board --
went down twice within ten minutes. Both were the shared station, and both
are fixed in `common/xprs_app`:

- **`idx` missed the task watchdog** and the board rebooted with "idx did
  not reset the watchdog" while the UI task happened to be running. The
  index task drains what the radios heard with one flash write per packet
  and reset the watchdog once per pass; when four stations refill the queue
  faster than flash drains it, the pass runs the whole 90 s. It now feeds
  the watchdog per packet.
- **The UI task overflowed its 6,144-byte stack** (in the core dump: "A
  stack overflow in task ui has been detected"). It is 8,192 now.

Neither is about the Heltec. They were there to be found by any fourth
station, and a fourth station is what this board is for.

## Measured on the bench, 2026-08-30

The other LoRa stations on the bench were the SenseCAP P1-Pro (X3S7S8) and,
until it was borrowed for other work, the T-Deck (X3GSLC). All figures
were taken with no serial port open on the counting side except the
Heltec's own CP2102, which does not reset it.

| | |
|---|---|
| Heltec → P1-Pro, LoRa only | **19 of 20** messages sent with `bearer=lora` (two runs of ten, 8-9 s apart), every one at −21 dBm on the P1-Pro |
| P1-Pro → Heltec, LoRa | every P1-Pro packet in a 15-minute window arrived direct: 5 of 5, all −16 dBm SNR 12 |
| Heltec → T-Deck, LoRa only | **10 of 10** messages sent with `bearer=lora`, −35/−36 dBm on the T-Deck |
| T-Deck → Heltec, LoRa only | **10 of 10**, −34/−35 dBm SNR 12 here |
| Heltec digipeating | 9 of the T-Deck's 10 LoRa sends re-aired by this station with `via:...,X3H3MZ` appended (`digi_on` set); the P1-Pro also received its own beacon back at −21 dBm relayed by this station |

Two things about counting LoRa on this bench. Beacons cannot be counted:
every station here also hears every other on BLE5, the BLE copy lands
first, and the LoRa copy is swallowed by the duplicate ring before anything
logs it as a reception -- which is correct behaviour and useless as a
figure. So the counted runs are bearer-pinned sends (`POST /api/xprs/send`
with `"bearer":"lora"`), which never touch BLE. And `scope:local` messages
are not relayed by anybody, so a round trip through a digipeater needs a
message without it; ten of the first batch went out local and came back
from nowhere, exactly as specified.

## Verifying it

A screenshot over the CP2102:

```sh
python3 ../../tools/scripts/framedump.py --port /dev/ttyUSB0 --cmd S /tmp/heltec.png
```

Over the network, with no serial port open (`docs/esp32.md` is binding):
`/api/status` for the callsign and `uptime_s`, `/api/xprs/history` for what
it heard on which bearer, and the T-Deck's history for what it heard from
this board on LoRa.
