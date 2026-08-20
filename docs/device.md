# What an XPRS ESP32 device does

The reference implementation is `models/m5stack-core`; the T-Dongle S3
(`models/tdongle-s3`) is the same station on smaller hardware. A new board
is expected to provide the functionality below, reusing the common/
components that already implement it. Section numbers refer to the XPRS
specification (docs/XPRS.md).

## 1. Bearers

At least one short-range bearer, per the chip's ability (radio table in
esp32.md): ESP-NOW (`geogram_xprsnow`), WiFi/LAN UDP (`geogram_xprslan`),
BLE5 extended advertising on -S3 chips (subtype 0x58). Everything heard on
one bearer is offered to the others; the bearer components own the via:
discipline, the random re-air delay and the 13.2.1 stand-down.

A periodic `t:observation` beacon reports what this station directly hears
(10.6): only stations heard WITHOUT a via: qualify for `hears:`.

## 2. Station duties

- `t:identity` with the station's npub, signed, on every bearer, roughly
  every 10 minutes (9.3) -- what makes every other signature checkable.
- Answer `t:ping` with `t:pong` carrying the measured rssi, rate-bounded
  (11.6, 31.2).
- Relay with the via: discipline (13): append the own callsign, refuse
  when already in the path or the type's hop budget is spent, stand down
  when somebody else airs the packet first.
- Answer `cmd:history` within the serving budget (25.2, 31), on the
  bearer the ask arrived on.
- When storage exists (SD card or a flash FAT partition): index every
  heard packet (36, `geogram_xprsindex`), verify signatures against heard
  identities, announce `t:service serve:archive` with the archived
  count (36.9), and serve the XDIR1 directory.
- Where a mesh custody plane exists (BLE5 boards): park store-and-forward
  mail for absent stations and release it on sight (`geogram_blemesh`).
- Sign what the station says when it holds a key (`geogram_xprssig`);
  a keyless station transmits unsigned and says so (37).

## 3. HTTP API

`common/xprs_api` is the reference surface: `/api/status`,
`/api/xprs/history` (the spool, newest first), `/api/xprs/send` (validated
caller-composed wires), `/api/xprs/dir`, `/api/log` (the rotating log,
machine-stamped). Boards with the walk-up hotspot add `common/xprs_hotspot`
(the captive chat page). httpd runs on core 1.

## 4. Screen

The minimum, whatever the size: who is in reach, how much traffic went by,
and the recent chat -- the three stores `common/xprs_station` keeps
(devices ring, wall-clock stats buckets, chat ring). Big screens render
them with `common/xprs_ui` (the m5stack's eight panels); tiny screens with
`common/xprs_ui_mini` (the T-Dongle's three views, auto-rotating every
10 s because its one button is barely reachable). Both UIs answer the
FRAMEDUMP serial screenshot, decoded by `tools/scripts/framedump.py`
(m5stack: the 'S' key; t-dongle: the `dump` console command).

Feed the stores from every RX path (`xst_ingest_parsed`), from non-XPRS
sightings such as RNS announces (`xst_dev_note`), and bank transmit totals
once per render tick (`xst_tx_total`). Persist the stats blob wherever the
board keeps storage (`xst_stats_load/save`); only one task writes it.

## 5. Configuration

`common/xprs_config` (config.ini on the storage, shared over the LAN):
`ssid`, `pass`, `name`, `nsec` (the signing key), `ntp`, `tz`, and the
feature toggles `wifi_on`, `espnow_on`, `digi_on`, `bridge_on`, `igate_on`,
`index_on`, `ap_on`. A board without the config share keeps the same keys
in NVS.

## 6. Discipline

docs/esp32.md is binding and measured: core 0 belongs to the radios, core
1 to anything that blocks (SD writes, the index writer, httpd); big task
stacks are claimed at the top of app_main and every xTaskCreate result is
checked; FatFs has one writer per store; the heap symptom table is where
every mystery failure has ended up so far.
