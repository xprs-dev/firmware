# ESP32-C3 mini

The smallest station in the fleet: a single-core RISC-V ESP32-C3 with 4 MB
of flash, no PSRAM and no screen. It carries XPRS over **BLE5, ESP-NOW and
the LAN**, repeats and bridges between them, serves the HTTP API, and runs
the walk-up hotspot with the chat page, which is how a person uses a board
that has no screen and no keyboard.

The station is `common/xprs_app`, the same program every other board runs.
`firmware/` holds only what makes it this board, and there is very little of
it.

## What is in here

| | |
|---|---|
| `firmware/` | this board's PlatformIO project |
| `board.yml` | the catalogue entry, machine-readable (`docs/catalog.md`) |
| `docs/station-plan.md` | the plan written before the work; what happened is below |
| `sdkconfig.esp32c3_mini` | the OLD multiboard build's config, not used by `firmware/` |

## The bench board

Chip ESP32-C3 revision v0.4, 4 MB flash, native USB (USB-Serial-JTAG),
MAC `48:F6:EE:E2:E9:6C`, callsign **X30Y64**. The native USB and the
single LED on GPIO8 suggest an ESP32-C3 **SuperMini** rather than the
DevKitM-1 the old files describe. The LED and the BOOT button (GPIO9) are
not used yet.

## Building and flashing

```sh
cd firmware
~/.pio-venv/bin/pio run            # build
~/.pio-venv/bin/pio run -t upload  # flash
```

Copy `src/wifi_secrets.h.example` and `src/fw_secrets.h.example` to the
names without `.example` first; both are gitignored. `platformio.ini` pins
the port by this board's USB serial number.

**Coming from other firmware**: the board arrived with an Arduino-style
table (`otadata` at 0xE000, apps from 0x10000), which overlaps this one's NVS
and `otadata`. Clear the overlap once after the first flash:

```sh
python3 -c "open('/tmp/ff12k.bin','wb').write(b'\xff'*12288)"
esptool.py --port <port> --no-stub write_flash 0xE000 /tmp/ff12k.bin
```

**Changing `sdkconfig.defaults`**: delete `firmware/sdkconfig.c3mini`
before building. A default only fills keys the generated file does not
already have, so a new line is silently ignored otherwise.

## What had to change for this chip

**One core.** Six places in the shared code pinned tasks to core 1, and on
a C3 that is an assert and a boot loop. They now use `XPRS_WORK_CORE`
(`common/xprs_common/include/xprs_core.h`): core 1 where there is one, no
affinity where there is not.

**No screen.** `display_init = NULL` is a board shape now, and the board
links `common/xprs_ui_none`, a UI that draws nothing and brings no LVGL. The
station still runs its UI task, without rendering, because that task is the
serial console: `cfg set ...` works over the USB cable.

**RISC-V.** The crash summary has no walked backtrace on this architecture;
`xprs_diag` reports the return address instead.

**RAM.** The first boot ran out: 1,612 bytes free after the hotspot, then
a panic. Two things got it back:

| | before | after |
|---|---|---|
| code in IRAM (the same SRAM as the heap on a C3) | 95,374 | 56,802 |
| static `.bss` | 109,720 | 82,696 |
| heap before BLE | 114,652 | 180,484 |
| heap after the hotspot | 1,612, then a panic | 55,344 |
| running, lowest seen after serving pages | | ~58,000, 35,808 |

The IRAM figure is ESP-IDF's own "minimizing RAM" switches in
`sdkconfig.defaults`. The `.bss` figure is shared code: the table rows and
chat copies only a screen's render path uses are now taken from the heap
on first use, so a board with no screen never pays for them.

**One radio, weak signal.** On the bench this board hears the router at
-84 to -92 dBm (the e-paper board beside it: -62). With BLE up, web pages
stalled after their headers while pings mostly got through: at the lowest
WiFi rates a full-size frame is longer than the gaps BLE leaves on the
shared radio. Two settings fixed it, measured with one ping a second for
three minutes:

| configuration | pings of 180 | the 22 KB chat page |
|---|---|---|
| BLE scanning 83% of the time | 108 to 115 | never completes |
| BLE off | 164 | 0.16 s |
| short BLE scan (21%) | 133 | never completes |
| short scan + 536-byte TCP segments | **172, 174** | **0.2 to 5 s, always complete** |
| 536-byte segments, long scan | 170 | 1.2 to 7.4 s |
| transmit power capped at 8.5 dBm | 54 | |

The transmit-power cap is the fix usually suggested for SuperMini boards;
here, far from the router, it made things much worse. The rest of the gap
is placement: nearer the router this board would do better on every line.

## Storage

The 832 KB `storage` partition holds the log, the statistics and the
conversation, and no archive: one index segment is 1.3 MB and could never
be evicted from a volume that small (`storage_mount` in `firmware/src/main.c`).

## Measured, 2026-09-11

| | |
|---|---|
| image | 1,219,144 bytes, 77.5% of a 1.5 MB slot |
| static RAM (`.data` + `.bss`) | 106,472 bytes |
| heap: before BLE / after BLE / after WiFi / after API / after hotspot | 180,484 / 141,420 / 104,336 / 90,872 / 55,344 |
| roster | http api, lan bearer, esp-now, wifi address, archive (none, by design): all up |
| heard by | the e-paper board, over BLE (-71 dBm) and the LAN |
| with no LAN (a missing SSID), hotspot and BLE up | ESP-NOW still received (15 packets in 15 s) |

Not yet checked: a phone joining the hotspot and loading the chat page over
it (the page itself is served, and loads over the LAN).
