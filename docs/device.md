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

## 6. Updating and diagnosing without a cable

A station on a roof updates over the air or stays on the version it was
carried up with (XPRS.md 25.8). What that needs on the device:

- Two app slots and rollback (`otadata` + `ota_0` + `ota_1`,
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`), so a new image that cannot
  come up is replaced by the one that could. Never a `factory` slot: it
  only helps when otadata is corrupt, and it goes stale.
- `common/xprs_ota` for the install and `common/xprs_auth` for the
  question that comes first -- may this signer make this station act
  (25.4). Both are board-agnostic; a board supplies its id, its callsign,
  a way to air an answer and a way to quiesce its storage.
- A pinned publisher key and an owner allow-list (`fwkey`, `own1..own4`
  in config, seeded from a gitignored `fw_secrets.h`). Both re-writable
  with a cable: a lost key is a ladder, never a brick.
- `/api/diag`, `/api/log` and `/api/coredump`, so the questions a person
  would climb up to answer can be asked from the ground.

**Not every board can host the pull path, and this is measured.** The
m5stack-core is an original ESP32 running LVGL beside WiFi, a FAT
archive, an indexer and a captive portal, and it lives at about 8-15 KB
of free heap. An HTTP client for a 1.4 MB image does not fit in that
comfortably: the updater's own 8 KB task took the whole margin (ENOMEM in
the LAN bearer, the index unable to open its files), and buying the
memory back by trimming LVGL's pool to 24 KB starved the renderer into
missing its watchdog every seventy seconds. The board keeps its 32 KB
pool and its stability.

So the rule for a new board: **an OTA host needs roughly 25 KB of free
heap at rest, or it needs to receive its image rather than fetch it.**
The T-Dongle S3 has the room (671 KB spare in its slot, 13 MB of unused
flash, no LVGL pool contention, ~22 KB free); the m5stack takes a push
over its own access point instead, which costs no HTTP client at all.


## 6.1 What was actually proved, and on what

Every line below was run against a T-Dongle-S3 on the bench, over the
network, with no cable touched after the image was built. Each result is
from the station's own log or its /api/diag, not from inference.

| case | expected | observed |
|---|---|---|
| push with no auth header | refused, nothing written | 403, still on the old version |
| push with a forged auth (real callsign, wrong key) | refused, nothing written | 403, still on the old version |
| unsigned cmd:update over the air | silence -- no answer at all | no reaction of any kind |
| stale + forged cmd:update over the air | silence | no reaction of any kind |
| signed cmd:update over the air | acted on, answered | verified on relay_task, answered `code:501 push the image to me` (this board has no fetcher) |
| push, valid operator, TAMPERED image approval | full transfer, refused at the end, boot partition untouched | `approval does not verify` -> `push refused: no valid approval` -> station back up on the old image |
| push, valid operator, valid approval | installs, reboots, self-test commits | 1,445,424 B in 38 s -> `ota_1` -> `this image proved itself -- rollback cancelled` -> state VALID |
| an image that condemns itself | previous image comes back on its own | `condemning this image` -> `going back to the one that did` -> reboot -> `rolled back from 0.2.8-badselftest: still running 0.2.7` |

Two honest gaps. The `code:500 fw:<old> m:rolled back` packet is
implemented and its cross-boot NVS record is written, but it was not
observed **on the wire**: an HTTP push carries no XPRS reply address, so
there is nobody to answer, and the over-the-air path that would carry one
cannot reach an install on this board because it has no fetcher. It needs
a board with `CONFIG_XPRS_OTA_PULL=y` -- the m5stack -- to be seen.

And the m5stack's own OTA has not been installed over the air at all. It
has the same code and the same receive-path split, and its heap is not the
dongle's, but "same code" is not a test.

**The rollback path has a build hook, on purpose.** It is the only part of
the updater a working image cannot exercise, and it is the part that
decides whether a bad push costs a ladder:

```sh
pio run -e rns_ble5 --build-flag=-DXOTA_FAIL_SELFTEST
```

That image condemns itself at the self-test. Sign it, install it, and
watch the previous one come back. Put the flag in the version string too,
so a shipped one is obvious.



## 6.2 Both boards are given their images; neither fetches one

Section 6 put the floor for a station that fetches its own image at about
25 KB of free heap. Measured against that floor, neither shipping board
clears it, and the reasons are worth writing down because they look like
bugs and are not.

**T-Dongle-S3.** Runs at about 14 KB free with everything up. The fetching
half of the updater is compiled out (`CONFIG_XPRS_OTA_PULL=n`); a
`cmd:update` naming no image is answered honestly with `code:501 push the
image to me` rather than going quiet.

**M5Stack Core.** Runs at 9-12 KB. It keeps the fetcher compiled in and
the fetcher gets as far as reading and parsing the manifest -- then
`esp_https_ota_begin()` answers `ESP_ERR_NO_MEM` and the failure is aired
as `code:500`, which is the system behaving correctly about not fitting.

The hotspot looked like the way out of that and is not. `heap after
hotspot: 30,448` in the boot trace, down from 139,440, so the SoftAP and
its DHCP server and netif cost roughly 109 KB -- but dropping to
`WIFI_MODE_STA` returns only **3,312** of them (7,628 -> 10,940 measured).
The rest is claimed when the interface is created and a mode change does
not give it back. Freeing it properly means tearing the netif down, which
has not been done.

So the push door is the path on both boards, and it is validated on both:

| board | image | transfer | outcome |
|---|---|---|---|
| tdongle-s3 | 1,445,424 B | 38 s | `ota_1`, self-test committed |
| m5stack-core | 1,459,120 B | 17 s | `ota_1`, self-test committed |

Two things had to change before either would accept a push, and both are
the same mistake in different clothes -- asking for memory at the moment
there is none:

- **The socket has to be patient.** `recv_wait_timeout` defaults to five
  seconds, which is sized for a JSON request, not for 1.4 MB arriving
  while the same task erases flash with the cache off. Pushes died at
  44-130 KB with `recv=-3` while every write returned `ESP_OK`. Thirty
  seconds on both boards.
- **The station has to stand down.** `quiesce()` originally paused the
  index writer and nothing else, so the transfer competed with the hub
  link, ESP-NOW and the bearers for lwip buffers; the window shut and
  never reopened. It now hands back real resources for the duration, and
  resumes on the failure path too.

And `CONFIG_SDCARD_MAX_FILES` is now per board, because each open file is
a 4 KB sector cache and that is the largest single lever either board has:
the M5Stack went from 6,796 to 12,132 bytes free by going from five to
four, which is the difference between refusing a push and taking one.


## 7. Discipline

docs/esp32.md is binding and measured: core 0 belongs to the radios, core
1 to anything that blocks (SD writes, the index writer, httpd); big task
stacks are claimed at the top of app_main and every xTaskCreate result is
checked; FatFs has one writer per store; the heap symptom table is where
every mystery failure has ended up so far.
