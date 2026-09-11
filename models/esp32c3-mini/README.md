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

## Setting it up from a phone

The published image has no owner and no network, and says so: a quarter of a
minute after it starts, and then every half minute on Bluetooth and every
two minutes on the LAN side, it airs `q:owner` with its own key (XPRS.md
11.9). The XPRS app's **Firmwares** screen lists it under *Waiting for an
owner*; from there it is claimed, given a WiFi network (the password sealed
to the board's key), a name and a time zone, and its stats read (11.10).

Over Bluetooth the claim has to come from the phone itself: a claim relayed
by another station carries `via:` and is refused, as 11.9 requires. The
setup below was run over the board's hotspot, which is also how a board
without Bluetooth is set up. What the Bluetooth leg showed on this bench is
further down.

| Step, 2026-09-11 | What happened |
|---|---|
| phone joins `XPRS-X30Y64` | the ask arrives over the LAN lane, the station is listed |
| claim | `station claimed by X1ARKL (11.9)`, `200 owner:X1ARKL` |
| stats | `cmd:zdiag` answered: firmware, uptime, free memory |
| name and zone | `200 ... nick:bench-c3 zone:+02:00` |
| WiFi, sealed | opened on the board, joined in about a second, `202 wifi:joining` then `200 wifi:up ip:192.168.178.145` |
| a new key (phone back on the home network, the board on it too) | `202 k:`, a restart, and the phone followed it to X3P5UQ; the board was then put back to X30Y64 from a copy of its NVS |

Two faults this found, both fixed in the shared station: with no network set
the board restarted every half second (a connect for an empty network name
made the channel set fail, and that was fatal), and once the board had joined
a network its broadcasts stopped reaching the phone on its hotspot, so every
answer to a LAN command now also goes straight to the address it came from.

**The Bluetooth leg, 2026-09-11.** The phone hears this board: it listed the
board from its Bluetooth ask and relayed that ask onward (`via:X1ARKL`). The
board does not hear the phone. In four minutes it decoded 67 Bluetooth frames,
every one aired by the e-paper station X3MEAV at -95 to -99 dBm and none by
the phone, so the phone's claim reached it only as X3MEAV's relayed copy and
was refused. The ESP-NOW frames it hears arrive at -90 dBm as well, so this
board receives everything weakly, whichever radio.

One fault on the board's side was found and fixed: it asked for the short
Bluetooth scan window (21% of the time, `ble_scan_light`) from boot, to keep
a weak WiFi link's pages fast, and so listened at a quarter of the time even
with no WiFi at all, which is exactly when a fresh board is set up. The short
window now applies only while the board has a WiFi link. Bluetooth frames
heard went from about 4 a minute to about 22 with ESP-NOW on and 40 with it
off.

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
