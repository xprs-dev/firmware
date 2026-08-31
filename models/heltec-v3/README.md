# Heltec WiFi LoRa 32 V3

An ESP32-S3 with an SX1262 on 869.5 MHz and a 0.96" 128x64 OLED, the size of
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

### The top bar says what the antenna is doing

The strip has no bottom bar -- the mini UI's three views are all body, and
the button legends the big UI draws belong to boards with buttons under the
screen. What this board has instead is one line of chrome, and on a LoRa
board the uptime is the least useful thing to spend it on: the number
somebody actually wants while pointing an antenna is the last packet's
signal, and it is invisible without a laptop.

So the bar reads `Devices  -35dBm  3 dev`, refreshed every two seconds, and
`LoRa quiet` when nothing has been heard for five minutes. A board without a
radio (the T-Dongle) sets no note and keeps the uptime it always had.

### The button

PRG is the strap pin, active low. The T-Dongle's grammar, unchanged: tap =
next view (and the first tap stops the tour), hold 0.7 s = home, keep holding
to 2 s = restart the tour. The board asks for the tour at boot
(`.rotate = true`) for the same reason the T-Dongle does: one button.

### The screenshot door

`GET /api/screen` answers with a BMP of what the panel is showing -- a
54-byte header and the pixels, streamed a segment at a time so the cost on
the board is one row of scratch and not a framebuffer. It is board-agnostic
(`xprs_api`, `xui_capture`): the T-Deck serves its 320x240 in 4.5 s.

**On this board it is unreliable**, and honestly so: at about 10 KB free the
TCP stack cannot keep a 24 KB transfer moving and the request usually ends
empty. Here the screenshot to use is the UART framedump, which the console
fix below finally made possible:

```sh
python3 ../../tools/scripts/framedump.py --port /dev/ttyUSB0 --cmd S out.png
```

### The console, and why nothing answered it

`getchar()` on a plain UART console BLOCKS when no driver is installed, and
IDF installs none. The UI task reads one character a tick, so on this board
it stopped on the first read and never looped again: the screen froze on its
first frame, the button did nothing, and any screenshot request waited for a
repaint that was never going to come. `xapp_run()` now puts stdin in
non-blocking mode for every board -- the ones with native USB were
non-blocking by luck, not by design.

### What this board runs at

No PSRAM, so internal heap is the whole of its room, and this is the
T-Dongle's config with two changes and the LoRa bearer added:

- `CONFIG_XPRS_BEARER_RNS=n`. The hub link's one TCP socket and task cost
  the T-Dongle 12,676 bytes; this board finished its first boot with 6,440
  free and a min-ever of 16, and it does not dial a hub.
- `CONFIG_BT_CTRL_BLE_MAX_ACT=2` (the T-Deck's value) and the LVGL pool at
  12 KB, since 128x64 is 64% of the T-Dongle's pixels.

Measured 2026-08-31, with everything running: heap after BLE 109 KB, after
WiFi 50 KB, after the API 37 KB, and about **10 KB free at steady state**
once the UI task (6 KB of stack), LVGL's 6 KB pool and its ten-row draw
buffer are paid for. `.heap_floor = 4000`.

Two figures from the evening before are struck out because they were taken
on a board that was not doing the work: 14-15 KB free was measured while
`xTaskCreate(ui_task, 8192)` was silently failing -- there was no UI task at
all, and a station with a screen and no UI task repaints nothing and answers
no console key. That is what the fallback and its log line exist to prevent.

**This board is at its limit**, and the limit is TCP: at 10 KB free, `ping`
is 20 of 20 but a bulk HTTP transfer crawls. Everything on the air -- the
four bearers, the digipeater, the beacons -- is unaffected, and that is what
this board is for. If it needs to serve the LAN properly, the archive is the
thing to turn off (`cfg set index_on 0`): its FAT sector caches are 4 KB
each.

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
  stack overflow in task ui has been detected"). It asks for 8,192 now and
  falls back to 6,144 where there is no contiguous block that size -- which
  is this board, and where the first version of the fix stopped the UI task
  from starting at all.
- **The UI task had no core affinity.** `docs/esp32.md` says anything that
  blocks for milliseconds belongs on core 1, and a panel flush is exactly
  that: on this board it is 8,192 pixel writes and a 1 KB I2C transfer. Left
  unpinned it ran beside the BLE controller and WiFi on core 0, and this
  station answered ping not at all. Pinned to core 1: 20 of 20.

Neither is about the Heltec. They were there to be found by any fourth
station, and a fourth station is what this board is for.

## Measured on the bench, 2026-08-30

The other LoRa stations on the bench were the SenseCAP P1-Pro (X3S7S8) and,
until it was borrowed for other work, the T-Deck (X3GSLC). All figures
were taken with no serial port open on the counting side except the
Heltec's own CP2102, which does not reset it.

| | |
|---|---|
All of the 2026-08-30 figures below were taken at 868.0 MHz; the fleet moved
to 869.5 MHz (band g3) on 2026-08-31 and the link was re-measured there.

| 868.0, 2026-08-30 | |
|---|---|
| Heltec → P1-Pro, LoRa only | **19 of 20** messages sent with `bearer=lora` (two runs of ten, 8-9 s apart), every one at −21 dBm on the P1-Pro |
| P1-Pro → Heltec, LoRa | every P1-Pro packet in a 15-minute window arrived direct: 5 of 5, all −16 dBm SNR 12 |
| Heltec → T-Deck, LoRa only | **10 of 10** messages sent with `bearer=lora`, −35/−36 dBm on the T-Deck |
| T-Deck → Heltec, LoRa only | **10 of 10**, −34/−35 dBm SNR 12 here |
| Heltec digipeating | 9 of the T-Deck's 10 LoRa sends re-aired by this station with `via:...,X3H3MZ` appended |

| 869.5, 2026-08-31 | |
|---|---|
| T-Deck → Heltec, LoRa only | **10 of 10** distinct messages, all −36 dBm SNR 12 |
| Heltec → T-Deck, LoRa | five of this station's relays received at −35..−37 dBm (`via:X3H3MZ`); the counted origin run was blocked by this board's HTTP intake, not its radio -- the send door stopped answering under bench load, a known limit of its ~10 KB heap |
| The governor, tripped on purpose | with `lora_duty_ms 4000` / `lora_resv_ms 1000`: held exactly at the 3.0 s ordinary cap ("held -- 2.9 s of 4.0 s spent this hour", 123 times), while a T-Deck sos was relayed the same second on LAN, BLE and ESP-NOW (`via:X3H3MZ`); its LoRa reserve copy was 13.2.1-cancelled because the P1-Pro relayed it first, which is the mesh working, not the governor failing |

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

## The radio's manners

Since 2026-08-31 the fleet sits at **869.5 MHz** -- ERC 70-03 band g3
(869.40-869.65), 10% duty cycle and up to 27 dBm e.r.p. -- where 868.0 had a
125 kHz channel straddling band g1's floor while paying g1's 1%. The bearer
carries a duty ledger: real airtime per packet (390 ms full-size at SF7)
charged against a rolling hour, ordinary traffic held when the budget is
spent, 6 s reserved so an sos still leaves, priority first out of the queue.
`/api/status` shows the ledger; the top bar reads `held 41s` when the budget
rather than the band is what is silent. `cfg set lora_region eu-g1` moves a
station back to the old sub-band; docs/API.md has the whole table.
