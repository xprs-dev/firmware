# xwifi instead of ESP-NOW, re-evaluated

Status 2026-11-09: **an evaluation and a plan, nothing implemented.** xwifi was
retired on 2026-10-25 (cc94077) because the full station plus xwifi does not
fit the ESP32-C3. Since then the no-PSRAM work of 2026-11-07 (c91498e,
8e23031, `docs/footprint.md`) gave the boards a lot of heap back. The question:
does that change the verdict, and would xwifi give ESP-NOW's packets longer
range?

Sources: the retired plan (`git show cc94077^:docs/xwifi-implementation.md`),
the xwifi README (`../xwifi`, last commit 2026-10-23), `docs/footprint.md`,
`docs/espnow.md`. Parked code: branch `origin/xwifi-wip` (bearer
`common/xprs_bearer_wifi8`, Heltec lean profile, C3 relay env) and the private
`xprs-dev/xwifi` repo.

## 1. The range: what xwifi buys

| | TX power | RX sensitivity | link budget | vs ESP-NOW |
|---|---|---|---|---|
| ESP-NOW, 1 Mbps (today) | up to 20 dBm | about -98 dBm (802.11b 1 Mbps, datasheet) | ~118 dB | - |
| ESP-NOW, LR PHY (250/500 kbps) | up to 20 dBm | a few dB better (Espressif's figure, not measured here) | ~121-123 dB | +3 to +5 dB |
| xwifi, profile E10 (what the C3 port may use today) | 10 dBm | -111 dBm (12s and 3s, window receiver) | ~121 dB | **+3 dB** |
| xwifi, profile E18 (needs listen-before-talk energy sensing, not in the C3 port) | 18 dBm | -111 dBm | ~129 dB | **+11 dB** |

- +3 dB is about 1.4x the distance in free space and 1.25x among buildings.
  +11 dB is about 3.5x and 2.3x.
- So **xwifi only wins clearly with E18**, which needs the energy sensing
  written for the C3/S3 port, and a measured output power (the DAC amplitude
  was never calibrated against an SDR). At E10 it is roughly what ESP-NOW's
  long-range PHY would give for free.
- The price on air: a packet takes 1.5 s, 3 s or 12 s (ESP-NOW: about a
  millisecond). Fine for messages and beacons, not for bulk.
- No outdoor range test was ever run: the field test and the T-Deck range
  test exist, but every number above for xwifi is from the desk and simulation.
- ESP-NOW today only reaches stations on the same channel, which in practice
  means the same access point, where the LAN bearer already connects them. Its
  own range (the LR PHY on a working channel, spec section 23.7 `t:channel`) is
  not built yet either.

## 2. The ESP32-C3: does the full station plus xwifi fit now?

**No. The new heap does not close the gap.** Three separate limits, each
enough on its own:

1. **RAM.** The radio's sample bank takes a fixed 128 KB of the C3's SRAM
   (0x3FCA0000-0x3FCBFFFF) whenever xwifi is linked.
   - On 2026-10-23, with the bank reserved, BLE off and the index off, the
     station reached WiFi with 17.6 KB free, and WiFi needs about 37 KB.
   - The 11-07 trims moved static RAM 91 -> 73 KB and gave about 43 KB of free
     heap with everything running. Most of the static saving is buffers now
     claimed on use, so the full station claims them again. Being generous,
     20-25 KB more before WiFi: WiFi then just fits, and the API (19 KB), the
     hotspot (47 KB) and Reticulum do not. The station is still 70-100 KB short.
   - Removing ESP-NOW gives back 2.4 KB: it rides the WiFi driver.
2. **CPU.** The C3 has one core. The xwifi receiver needs 10-17 windows a
   second at 55-135 ms of search each: 60-100 % of the core, with interrupts
   masked 1.2 ms per window and 7.84 ms per sent block. The web server, BLE and
   Reticulum share that core.
3. **The radio.** xwifi drives the PHY directly: WiFi and BLE cannot run while
   it does. Taking turns ("sliced" mode) is written but never ran on hardware,
   and a station taking turns is deaf on xwifi while it serves WiFi, and its
   hotspot clients lose it meanwhile.

So replacing ESP-NOW with xwifi on the C3 full station would cost the hotspot,
the internet link and BLE. ESP-NOW costs 2.4 KB and runs beside all of them.

## 3. What has changed for the other boards

- **T-Deck (S3, PSRAM): worth a retry.** The 10-23 attempt stopped at its third
  wall: FreeRTOS and heap code in flash were needed for 24 KB below the bank,
  and they looked fatal to the BLE controller (interrupt watchdog at boot). The
  T-Dongle has since run exactly that configuration (FreeRTOS, ring buffer and
  heap code in flash) with BLE5 scanning and the hotspot up. So the wall may
  have been something else, or T-Deck specific. The S3 also has a second core
  for the receiver's search.
- **Heltec V3 (S3, no PSRAM):** the same trims are in its defaults (not measured
  on a board). Its profile question (A modal, B bridge, C lean radio) can be
  reopened with real numbers.
- **The C3 as a dedicated radio:** the relay (xwifi + console) already worked
  on air in October.

## 4. Recommendation

Keep ESP-NOW on every station. Bring xwifi back as an addition where it fits,
and only if the outdoor test shows the range is real.

## 5. Plan

0. **Measure before building.** Outdoors, same pair of C3 boards, same
   antennas and places:
   - ESP-NOW at 1 Mbps;
   - ESP-NOW on the LR PHY;
   - xwifi 12s and 3s at E10.
   Use the existing field test, `tools/xw_bench.py` and CSV logs. Measure the
   xwifi output power with an SDR first, so the EIRP stated in `t:channel` is
   real. Outcome: distance at 90 % delivery for each.
1. **The cheap win: ESP-NOW long range.** If LR closes most of the gap, build
   section 23.7's working channel (`t:channel` invitation, both tune to a
   channel and the LR PHY, both come back), and decide what `scope:local` means
   there (`docs/espnow.md` leaves it open). No memory cost.
2. **E18 for xwifi.** Energy sensing (listen-before-talk) in the C3 and S3
   ports: it is the +8 dB that makes xwifi worth its price.
3. **xwifi where it fits:**
   - **The C3 as an xwifi radio modem**: the relay firmware plus a serial link
     (USB or UART) carrying XPRS lines to a host station: the Pi server, a
     T-Dongle, or the future ESP32 house station (`plan-esp32-home-station.md`).
     The host keeps WiFi, BLE, the archive and the web; the C3 only does the
     radio. Needs a serial bearer on the host (core for Linux, a UART bearer in
     the firmware); the relay's bearer with dedup and digipeating is reused as
     is.
   - **The T-Deck full station with xwifi**: rebase `xwifi-wip` on main (the
     bearer, `xw_scratch_t`, the event loop claimed at boot are partly merged
     already), apply the T-Dongle config block, retry wall 3, then run the
     sliced mode against a live access point. That run is the main risk.
   - **Heltec V3**: measure the trimmed image, then pick A, B or C.
4. **Release.** The xwifi library is private: the release workflow needs the
   pinned checkout back for the boards that link it (the stub path stays for
   the others), and `docs/wifi8.md` comes back from cc94077's parent.

Step 0 decides the rest: if xwifi at the power it may legally use beats ESP-NOW
LR by only a few dB, step 1 is the whole answer.
