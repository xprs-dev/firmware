# ESP32 firmware -- map & special characteristics

**Looking for what a device supports rather than how the firmware is built?**
[device-tdongle.md](device-tdongle.md) is the T-Dongle-S3's own page: its
bearers, what of XPRS it implements, what it deliberately does not, and how to
ask it questions.

Read this before touching `esp32/` -- it saves re-reading the tree. Covers the
project layout, which firmware is which, the BLE protocol state, the traps, and
the three constraints this board keeps punishing people for: **which processor
the work runs on**, **when the SD card is allowed to run**, and **how little
memory is left**.

## Three projects, one component library

| | Main multi-board project (`esp32/`) | `esp32/rns_ble5/` | `esp32/m5stack/` |
|---|---|---|---|
| Build | PlatformIO, `platformio.ini` with 8 envs (`pio run -e <env>`) | Own PlatformIO project, single env (`pio run` inside the dir) | same, `pio run` inside `esp32/m5stack/` |
| Framework | ESP-IDF **5.2.1** (espressif32@6.7.0) -- pinned, see memory note about needing a real framework dir | same | same |
| App | `src/main.cpp` (one binary, `HAS_*`/`FEATURE_*` gates per board) | `src/main.c` + `tweetnacl.c` | `src/main.c`, ~200 lines |
| BLE | **Legacy advertising only** (31 B) -- `xprs_ble_hello` | **BLE5 extended advertising** (`CONFIG_BT_NIMBLE_EXT_ADV=y`) | **none** -- this chip has no ext-adv |
| Boards | epaper-S3 (default env!), generic, C3, KV4P, Heltec v1/v2/v3, tdongle_s3 | T-Dongle-S3 (board id `esp32s3-devkitc-1`) | M5Stack Core, original ESP32-D0WDQ6, CP2104 at `/dev/ttyUSB0` |

`esp32/m5stack/` exists to be a **second voice on the air**: testing a bearer
with one device only proves that its loopback works. It shares the
communication components by symlink (`xprs_codec`, `xprs_bearer`,
`xprs_bearer_now`, `xprs_bearer_lan`) and runs XPRS over ESP-NOW and the LAN.
Its WiFi credentials live in a gitignored `src/wifi_secrets.h`, and they matter
for one reason: **ESP-NOW rides the channel the station is on**, so associating
to the same access point as the dongle is what puts both on the same channel
without anybody guessing one.

**The mesh/BLE5-capable dongle firmware is `rns_ble5`** -- the main project's
T-Dongle env is the older legacy-BLE APRS firmware. They cannot be merged
casually: NimBLE's legacy GAP API changes/goes away when `EXT_ADV` is enabled,
which is exactly why they are separate binaries.

Components live in `esp32/components/` (50+, prefix `xprs_*`); `rns_ble5`
reuses them via **symlinks in `rns_ble5/components/`** (PlatformIO fails on
`EXTRA_COMPONENT_DIRS` outside the project dir -- always symlink instead).
Component CMake gates by **IDF_TARGET, not CONFIG_** (early-expansion gotcha,
see `xprs_msgstore/CMakeLists.txt`).

## Radio capability per chip (mesh implications)

- **Original ESP32** (Heltec v1/v2, KV4P): **no BLE5 extended advertising** --
  those boards can never join the extended-advert mesh plane; legacy 31 B only.
- **S3 / C3 class** (T-Dongle-S3, Heltec v3, C3-mini): extended advertising OK,
  one AD structure <= **254 B** (`EXT_ADV_MAX_SIZE=1650` is the chain cap; we
  keep every frame in a single AD <=254 B so phones with ~247 B controller caps
  hear it).
- The dongle runs **one ext-adv instance**; `relay_task` rotates what's on air
  every 1.5 s: queued relayed frames first, else (idle) alternating our signed
  RNS announce and the mesh route beacon every 8 s.

## BLE wire protocols (all under company id 0xFFFF, marker 0x3E)

| Subtype | Meaning | Who |
|---|---|---|
| `0x55` | Reticulum packet (announces relayed blind, HEADER_2 hops+1) | rns_ble5 |
| `0x41` | APRS broadcast parcel, compact `from\x1F to\x1F text` | both projects + phones |
| `0x4D` | **street-mesh route beacon** (docs/mesh.md section 3) | rns_ble5 (`xprs_blemesh`) + phones |
| `0x47` | phone GATT presence beacon | phones only (not implemented on ESP32) |
| `0x50/0x51/0x52` | legacy broadcast-parcel chunks + NACK (13-17 B payloads) | legacy firmware + legacy-phone path |
| `0x42` | legacy SCAN_RSP continuation | legacy firmware |

Legacy firmware advert caps are compile-time (`ADV_MFG_CAP=20`,
`APRS_MFG_MAX=44` with SCAN_RSP); the phones' extended frames are simply
invisible to it.

## xprs_blemesh (the reusable mesh core)

`components/xprs_blemesh/` -- pure C, deps mbedtls+log only, **no radio/
storage/UI** (firmware wires those), so it ports to any ESP32 target:

- `blemesh_beacon.c` -- 0x4D codec, wire-compatible with
  `aurora/lib/services/mesh/mesh_beacon.dart` (ver 1, class byte, cond byte,
  3-byte SHA-256 callsign hash + cost DV entries, bloom slot reserved).
- `blemesh_table.c` -- neighbors (24) + DV routes (64), bidirectional-confirmed
  next-hops, cost cap 6, 300 s aging, DV export (neighbors at cost 1 first).
- `blemesh_scf.c` -- store-and-forward custody: parks heard 1:1 `0x41` frames
  keyed by their `am:` receipt id, purges on overheard `?ACK <am>`, re-airs
  when the target's beacon/frame is heard again (60 s per-sighting rate
  limit), 7-day TTL, optional stdio persistence (`/sdcard/mesh/pending.bin`).

Firmware glue lives in `rns_ble5/src/main.c`: scan demux -> `handle_mesh`,
`mesh_beacon_air` (class `esp32`, powered, stationary, storage bucket from SD),
SCF hooks inside `handle_aprs`, init in `app_main` after `igate_start` (the
mesh identity is the iGate callsign from NVS, fallback `TDONGLE`).

## Current-protocol rules the firmware honours (don't regress)

- **1:1 receipts**: messages carry a prepended `am:<6hex> ` token; receipts are
  `?ACK <am> d|r` frames. The dongle parks 1:1 frames by `am` and purges on
  `?ACK`. It never generates receipts (it is a carrier, not an endpoint).
- **Control frames never reach APRS-IS**: any text starting `?` (`?ACK`,
  `?PING`, `?MAIL`...) is not uplinked.
- **ENC1 ciphertext never reaches APRS-IS**: the phones deliberately keep
  encrypted 1:1 off the 7-bit APRS-IS air (it arrives as undecryptable
  garbage). The dongle checks the text after the optional `am:` token.
- Everything else is **relayed blind** (content-dedup ring 32/600 s), extending
  BLE coverage one hop -- including `?ACK` frames and ENC1 payloads over BLE.

## Ops / hardware traps

- T-Dongle-S3 flashes over native USB-JTAG (`/dev/ttyACM0`); after flashing it
  needs `--after hard_reset` (default) -- the port re-enumerates.
- LCD is ST7735 160x80 via LVGL 8.3.11 (`xprs_tdongle_ui`); LVGL is
  single-task -- UI updates only via the queue -> `ui_task`.
- SD is the T-Dongle's hidden microSD slot (under the USB-A cap); mounted at
  `/sdcard` via `xprs_sdcard` (SDMMC). Absent card must degrade gracefully.
- WiFi + BLE coexist: the ext scan runs at 60% duty (0x60 itvl / 0x50 window)
  deliberately, so WiFi (iGate) still gets airtime. That duty is about the
  handshake, not about throughput -- when WiFi collapses later, the cause is
  almost certainly the processor, not the antenna. See "The two processors".
- Secrets (`igate_secrets.h`) are gitignored; provisioning writes them to NVS
  on first boot and NVS is the source of truth afterwards.
- `build.sh` menu does NOT list tdongle_s3 or rns_ble5 -- build those directly
  (`pio run -e tdongle_s3` at the root, or `pio run` inside `rns_ble5/`).
- A full cold build takes >10 min (IDF from scratch); incremental is fast.
- **Three boards are on this bench and two of them answer to "the M5Stack" if
  you are careless.** `/dev/ttyACM0` is the T-Dongle (X3WWAJ, 192.168.178.102);
  `/dev/ttyUSB0` is the M5Stack Core (X3LTSH, 192.168.178.119); `/dev/ttyACM1`
  is X3R8XX, has an sx1262, and is neither -- though it logs under the same
  `xprs:` tag and is indistinguishable in a capture. An hour once went into
  instrumenting a code path that was never reached, because the command under
  test had been addressed to the board that did not have the firmware, while
  the console being read belonged to a third one. **Compare `uptime_s` between
  the serial `alive` line and `/api/status` before believing a capture belongs
  to the board you just flashed**, and address stations by the callsign the
  station itself reports.
- The M5Stack is a CP2104. `monitor-capture.sh -r` drives DTR/RTS in the
  combination that puts it into `DOWNLOAD_BOOT` and leaves it there; recover
  with `esptool --after hard_reset chip_id`, or reset it with RTS alone while
  holding DTR low.
- `version.txt` is read at CMake configure time. Editing it and running
  `pio run -t upload` flashes a NEW binary carrying the OLD version string,
  which then looks exactly like an upload that did not take. Check the build
  timestamp in `/api/diag`, not the version, to tell whether a flash landed.

## The two processors -- the rule that matters most

This chip has two cores and this firmware puts everything that matters on one
of them:

| Core 0 | Core 1 |
|---|---|
| BLE controller (`CONFIG_BT_CTRL_PINNED_TO_CORE 0`) | `xprsidx_wr` -- the index's writer |
| NimBLE host (`CONFIG_BT_NIMBLE_PINNED_TO_CORE 0`) | `rns_relay` -- signing, and the `cmd:history` replay |
| WiFi task (IDF default) | `rns_tcp`, `heartbeat`, the httpd worker |
| `app_main` and everything it calls (`CONFIG_ESP_MAIN_TASK_AFFINITY 0x0`) | |

`xTaskCreate()` leaves a task with no affinity, which is not the same as putting
it on core 1. **Anything that blocks for milliseconds at a time -- SD, FATFS,
crypto over big buffers -- must be pinned to core 1**:

```c
xTaskCreatePinnedToCore(task, "name", stack, arg, prio, NULL, 1);
```

### It came back, wearing a different hat

Writing this page down did not stop it happening again. `cmd:history` -- an
indexer answering "what have you heard" -- was written to answer inline, on
whatever task heard the ask. That put a FatFs query on the **NimBLE host task**
(priority 21, core 0) for a Bluetooth ask and on the LAN bearer's task for a UDP
one, and it put the replay's record reads on `rns_relay`, which was created with
a bare `xTaskCreate` and therefore had no affinity at all.

It did not look like this section. It looked like heap: `wifi:m f null`, the
station associating and dropping a minute later, `ext_adv_set_data rc=519` from
the controller, and a low-water mark of **756 bytes**. It was diagnosed as heap
and "fixed" as heap, twice, before anyone re-read this page.

What it actually needed was the split this section already prescribes -- the
receive task decides *whether* to answer (parse, dedupe, budget: all RAM), and a
core-1 task does the query, the signature and every reply -- plus one line
turning `xTaskCreate` into `xTaskCreatePinnedToCore(..., 1)`.

| | Before | After |
|---|---|---|
| Reachability, idle | -- | 120 of 120 |
| Reachability, during a replay | station dropped off | **120 of 120** |
| Free heap | 8,320 | 26,828 |
| Low-water mark | **756** | 19,996 |
| Largest free block | 7,680 | 14,336 |

The heap half of that came from `CONFIG_LV_MEM_SIZE_KILOBYTES` 32 -> 16: LVGL's
pool was the largest single buffer in the build and this display is a 160x80
text dashboard. Two things are worth taking from it. **A file handle is not
free** -- `CONFIG_FATFS_SECTOR_4096` with `CONFIG_FATFS_PER_FILE_CACHE` means
every open `FILE` holds a 4 KB sector cache, and a store that keeps three open
and a query that opens two more wants 16-20 KB it has to borrow from WiFi. And
**a task that fails to start says nothing**: `xTaskCreate`'s result was ignored,
so three kilobytes of new static silently meant no relay task at all -- no
beacons, no announcements, no replay, and a board that looked healthy from
every other angle. Check the result and log it.

### What this looked like when it was wrong

The XPRS index wrote a 320-byte record to microSD for every packet heard, from
the receive path. The WiFi station stayed associated and could not transmit:
`wifi:m f null` repeating, DNS timing out, the device unreachable. Measured by
pinging once a second:

| Configuration | Reachable |
|---|---|
| BLE on, SD on, writes on core 0 | **1 of 96** |
| BLE on, SD off | 159 of 162 |
| BLE on, SD on, writes on **core 1** | **178 of 182** |

### What did not work, so nobody repeats it

The symptom reads exactly like a radio problem, and the firmware's own comment
about the SDMMC bus desensitising the 2.4 GHz radio encourages that reading. All
four of these were tried against the 1-of-96 baseline and **none of them changed
anything**:

- `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` -- what ESP-IDF's own coexistence example
  marks "must call this"
- the BLE scan cut from 60% to 10% duty (20 ms window / 200 ms interval)
- `esp_coex_preference_set(ESP_COEX_PREFER_WIFI)` instead of `BALANCE`
- stopping BLE entirely (advertising, scanning and any open connection) for the
  duration of the WPA2 handshake

They are all defensible; none of them is the bug. The bug was the processor.

### The web server belongs there too

`config.core_id = 1` on the **httpd** task, for the same reason: every handler
on this board reads the SD card, and httpd has **one worker task**, so whatever
that task waits for takes down every endpoint rather than just its own.

Two consequences worth building around:

- a handler must never queue behind a batch of SD writes. `xprs_index`
  therefore takes its lock **per record**, and offers `xprsindex_pause_writes()`
  so a reader can hold the writer off the card for the length of a request and
  give it straight back. Records keep arriving into RAM meanwhile.
- an endpoint that stops answering is **not** evidence of a wedged server. On
  this board it is usually the station having dropped off the WiFi: `curl`
  reports a timeout, and `ping` -- checked in the same breath -- reports "no route
  to host". Check reachability before you debug the server. That confusion cost
  a day here.

## The SD card

- The T-Dongle's microSD is under the USB-A cap, SDMMC, mounted at `/sdcard`.
  An absent card must degrade gracefully -- several components run without one.
- **Never write from a receive path.** Decide on the caller's thread (parse,
  identify, deduplicate -- all cheap) and hand the finished record to a queue
  that a core-1 task drains.
- **Drain in bursts, not as a trickle.** A writer waking ten times a second
  keeps the FATFS layer busy and every other reader of the card queues behind
  it -- including unrelated endpoints like `/api/beacons`. Two seconds between
  bursts is enough to keep the card free the rest of the time.
- **FatFs is not thread-safe and this is not theoretical.** Two tasks adding
  records while two servers read them, through one `FILE*`, returns records that
  are half another task's seek. Stores need their own mutex.
- **A file's size lives in its directory entry, and FatFs writes that on sync or
  close.** Two consequences, both cost a day to find:
  - a second `fopen()` of the segment currently being appended sees the size
    from the last sync -- usually zero -- so records just written read back empty.
    Read the active file through the handle that is writing it.
  - nothing closes that handle when the power goes, so without an `fsync()` the
    whole active file is invisible after a reboot. 2000 records were lost to a
    reset exactly this way.
- **`CONFIG_FATFS_LFN` defaults to OFF, and 8.3 is smaller than you think.**
  The index names its segments `seg_<10 digits>.bin` -- fourteen characters --
  so on a build without long-filename support every segment `fopen()` fails.
  Nothing said so: the store's counter increments when a record is ACCEPTED
  into RAM, not when it is written, so the station reported "9 packets held"
  over a store with zero bytes on it and answered 404 to every history ask.
  Two lessons in one bug: `CONFIG_FATFS_LFN_HEAP=y` belongs in every board's
  `sdkconfig.defaults` that mounts a store, and when "held" and "served"
  disagree, read ONE record back (`xprsindex_get(st, 0, ...)`) before
  trusting either number.
  **And it happened again, to the board with the card.** 2026-08-25, the
  T-Dongle: `xprsidx: open /sdcard/xprs: 0 records, 0 segments` on a 29.28 GB
  card, announcing `serve:archive -- 19 record(s) held, 0 callsign(s)` in the
  same boot. LFN lived in `models/tdongle-s3/sdkconfig.tdongle_s3` -- which is
  the SHARED (multiboard) build's config for this board, not this one's; the
  env the firmware actually builds from,
  `firmware/sdkconfig.rns_ble5`, said `CONFIG_FATFS_LFN_NONE=y`, and
  `firmware/sdkconfig.defaults` said nothing at all. 17,715 records were
  sitting in five segments the firmware could not open. Check the generated
  config of the env you BUILD (`grep FATFS_LFN sdkconfig.<env>`), not the
  first sdkconfig with the board's name on it.
- **A board's sdkconfig fixes travel by hand.** The dongle had LFN on; the
  M5Stack regenerated its `sdkconfig` from its own `defaults` and silently
  did not. Anything a store or a driver needs (`LFN`, LVGL fonts, flash
  size) must be in THAT board's `sdkconfig.defaults`, because deleting the
  generated file to pick up a new default also discards every setting that
  only ever lived in the generated file.
- **The store does not need an SD card.** The M5Stack Core has 16 MB of
  flash and an app that uses 1.6 MB: a `storage, data, fat` partition after
  `factory` plus `esp_vfs_fat_spiflash_mount_rw_wl()` gives the index 14 MB
  of wear-levelled FAT and keeps NVS and the app at their old offsets, so
  a reflash loses nothing. Same FatFs, same traps as above.

  **The T-Deck is on that route on purpose.** It has a microSD slot, but it
  is on SPI behind `TDECK_SD_CS` (39), sharing the bus with the SX1262 at
  `TDECK_RADIO_CS` (9) and the panel -- while `xprs_sdcard` is SDMMC-only
  (`sdmmc_host_t`, 1-bit, `SDMMC_SLOT_FLAG_INTERNAL_PULLUP`). Using it means
  an `sdspi` driver AND arbitrating a bus the radio is on, which is the kind
  of coupling that turns "the archive is busy" into "the station missed a
  packet". Its `storage, data, fat` partition is 11.38 MB and its archive
  budget 10 MB. So the T-Deck can serve a super-archiver's ROLE but not a
  super-archiver's DEPTH -- 28k records against the dongle's card -- and the
  board with the card is the one to put beside a router.

## Heap is the binding constraint -- check it first

Every mysterious failure on this board so far has been heap. The symptoms do not
look like memory:

| What you see | What it actually is |
|---|---|
| `wifi:m f null` repeating | the driver cannot allocate its keepalive frame |
| ping fails while the log says the station is associated | lwip has no buffers |
| HTTP accepts TCP and never answers | the handler's 2-3 KB response `malloc` fails |
| `APRS-IS iGate init failed: ESP_FAIL` at boot | nothing left to start it with |
| a reboot every ~14 s | a task stack overflowed |
| `esp_now_send` returns `ESP_ERR_ESPNOW_NO_MEM` for every packet | the driver cannot get a send buffer |
| `BLE_INIT: Malloc failed` + `ext_adv_set_data rc=519` | the same, on the other radio |
| the station beacons never, answers nothing, and otherwise looks fine | a task never started -- see below |
| a debug/rare path silently does nothing (a screenshot dump emitting zero slices) | its one-shot `malloc` outgrew the heap and the `if (buf)` skips the whole feature |

**Create the big task stacks first.** `relay_task` asks for 8 KB in one piece.
It used to be created from `on_sync()`, which runs after WiFi, NimBLE, the SD
card, the HTTP server and the Reticulum hub have each taken their share: 15,308
bytes free but the largest block only **7,680**, so `xTaskCreatePinnedToCore`
returned `pdFAIL` and the station ran on with no beacons, no service
announcements, no `cmd:history` replay and no section 23.7 clock, while every other
task carried on and the heartbeat looked healthy. One `ESP_LOGE` at boot was the
entire evidence, and it scrolled past.

It is now claimed at the top of `app_main()`, where the heap is untouched
(172 KB in one block), and blocks on a flag until `on_sync()` releases it. The
heartbeat prints `relay=<loops>` so a task that stops is visible without
reading a boot log. **Check the return of every `xTaskCreate`, and prefer
claiming a large stack early over hoping it fits later.**

Measured heap by boot stage on the T-Dongle-S3 (`heap_mark()` in `main.c`), which
is how the above was found and the first thing to re-run when it recurs.

**Numbers in this file drift with the feature set, so they carry a date and a
configuration.** A table with no provenance is what let a whole `sdkconfig`
block go missing unnoticed for months -- see below.

*2026-08-21, LVGL pool 16 KB, hub link off, `CONFIG_SDCARD_MAX_FILES=3`:*

| after | free | largest block |
|---|---|---|
| boot | 208,208 | 139,264 |
| `nimble_port_init` | 141,724 | 77,824 |
| `igate_start` (WiFi STA + lwip) | 135,564 | 73,728 |
| `api_start` (httpd) | 73,140 -> 68,844 | 31,744 |
| BLE host + `gatt_mesh` | 62,804 | 31,744 |
| `sdcard_init` | 23,968 | 9,728 |
| the three XPRS bearers | ~8,200 | 3,584 |
| steady state, associated | **14,304** | 4,096 |

*Superseded (kept because the deltas are still the lesson): an earlier build
booted with 234,244 free and reached `api_start` with 39,792. The costs, not the
totals, are what carries over.*

WiFi (62 KB), NimBLE (47 KB) and the SD card (32 KB) are the three that matter;
everything else is noise beside them. Steady state after association is around
14 KB, so **there is no room for a new 8 KB anything** -- take it at boot or
reclaim first (`sdkconfig` buffer counts) and measure again.

Note the order in that table. `api_start` and the BLE host now come BEFORE the
card and the bearers, and that is not tidiness -- see "The boot order is the
allocator" below.

The whole cycle repeated on the M5Stack Core the day it grew an indexer, and
the same medicine worked: `xprsidx_wr`'s 8 KB start became a coin toss at
~29 KB free, so the index task moved to the top of `app_main` (heap still one
172 KB block) with its create checked and logged; LVGL's pool was sized from
`lv_mem_monitor()` (~20 KB used on a 320x240 table-and-chart dashboard, pool
36 KB) instead of guessed; the draw buffer went from a fifth to an eighth of
the screen; and `max_files` came back DOWN after being raised "to be safe" --
each open `FILE` is a 4 KB sector cache, so raising it to 12 had offered the
heap a 48 KB hole. Steady-state free heap went 29 KB -> 57 KB. A rarely-used
debug path (the serial frame dump) had also grown a 41 KB `malloc` nobody
re-measured; it failed silently the day the indexer moved in. Anything that
must not die quietly encodes in small static chunks instead of allocating
the whole answer.

A queue is heap too. `XPRSNOW_RX_QUEUE` was widened from 4 to 16 to stop
drops -- 264 bytes an entry, so four kilobytes out of the eight the board had --
and ESP-NOW then refused every transmission for want of a buffer while the drop
counter it was widened to protect sat at zero throughout.

BLE is the reason there is nothing left. Measured on the T-Dongle:

```
heap after nostr init:   164,516
heap after WiFi AP/STA:  120,848
heap after httpd start:  103,928
heap after BLE init:      11,432   <- NimBLE and the controller take ~90 KB
```

Eleven kilobytes is what the whole rest of the firmware had to live in, and any
task stack added to it took the station off the air -- 117 of 117 pings answered
became 1 of 86 by adding two background tasks.

**Where the 90 KB was hiding.** The defaults are sized for a busy central, not
for a station that advertises, scans and holds at most one connection:

| Setting | Was | Now | Frees |
|---|---|---|---|
| `BT_NIMBLE_MSYS_2_BLOCK_COUNT` | 24 x 320 B | 8 | ~5 KB |
| `BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT` | 24 x 255 B | 8 | ~4 KB |
| `BT_NIMBLE_HOST_TASK_STACK_SIZE` | 8192 | 5120 | 3 KB |
| `BT_CTRL_BLE_MAX_ACT` | 6 | 3 | |
| `BT_CTRL_SCAN_DUPL_CACHE_SIZE` | 100 | 20 | |
| `BT_NIMBLE_ROLE_CENTRAL` | on | off (we never dial out) | |

Free heap after BLE init went **11,432 -> 29,408**, largest block 7,680 -> 21,504.
With that, reachability is 137/137, the iGate connects for the first time, and
every HTTP endpoint answers including the SD-backed ones.

### The reclaim went missing, and nothing said so

Two years of this page had the table above, and the day the dongle grew an
updater the board would not answer HTTP at all. The updater was blamed first
(it had put a signature check on the receive task, which was a real bug and
is fixed), but the numbers did not add up: the pre-updater build was only
2,912 bytes of static RAM better off, and that cannot be the difference
between a working HTTP server and none.

`sdkconfig.defaults` was missing every line of the reclaim. The host task
stack was 8192 -- the "was" column. `MSYS_2_BLOCK_COUNT` and
`TRANSPORT_ACL_FROM_LL_COUNT` were absent, so the build had 24 of each. The
settings were lost somewhere in a repo move, no test covers a Kconfig value,
and the board ran for months on the margin they used to provide, which is
why 2,912 bytes was enough to end it.

What that looked like, and why it is worth recognising on sight: the station
kept gossiping perfectly over ESP-NOW and BLE while being completely
unreachable on the LAN. From the air it looked healthy. WiFi associated and
dropped every few seconds (`reason: 15`, the four-way handshake timing out),
`xprslan` logged `sendto failed: errno 12`, and the minimum-ever free heap
sat at **64 bytes**.

Measured on the T-Dongle-S3, boot to steady state:

| | before | after |
|---|---|---|
| free heap, steady | 2,788 | 11,116 |
| minimum ever | 64 | 1,264 |
| largest block | 960 | 3,584 |
| `/api/diag` | accepts TCP, never answers | answers |

Four changes got that back, in order of how much they returned:

1. **The reclaim table above, restored.** It was measured on this board and
   it still holds.
2. **`api_start()` moved ahead of BLE mesh, the SD card and the bearers.**
   Its 5 KB task stack is heap like any other. Asked for last it saw 5,312
   bytes free and a 2,816-byte largest block and `httpd_start()` failed;
   asked for before the three hogs it sees one 31 KB block. Nothing can
   arrive in the gap -- WiFi has no address for another second.
3. **One response buffer, claimed at boot and shared.** Every handler used
   to `malloc` its own 2 KB per request, at the exact moment in the board's
   life when 2 KB contiguous does not exist. `esp_http_server` runs handlers
   on a single task, so one buffer is enough, and if it cannot be had the
   server does not start -- an honestly missing server beats one that
   accepts connections and says nothing.
4. **lwip's TCP windows, 5,744 -> 2,880 each.** `rns_tcp_start()` opens one
   socket to one hub with a 4 KB task stack and cost 12,676 bytes; the other
   8.7 KB was that socket's two windows.

And the LVGL pool went 32 KB -> 16 KB, but only after `lv_mem_monitor()` was
added to `xprs_ui_mini` and reported 26% of 32 KB used, steady across all
three views. The M5Stack's 320x240 dashboard uses ~20 KB and a trim to 24 KB
there caused a 70-second reboot loop. Per board, per screen, measured every
time -- the number is not portable and neither is the confidence.

**The lesson that generalises:** a heap this tight has no alarm. It has a
minimum-ever counter, and nothing reads it until something breaks. The
`alive` line prints `min=` for exactly this reason; a board whose minimum
has three digits is already broken and has not noticed.

**The file that matters is `esp32/sdkconfig.tdongle_s3`**, not
`boards/sdkconfig.tdongle_s3`. The board fragment is a defaults seed; PlatformIO
maintains the generated one at the project root and that is what the build uses.
The fragment had asked for `MSYS_1=6` and no central role for who knows how
long, and the build had 12 and central enabled.

### Nothing here fails loudly

Four times now the station has been taken off the air by something that
returned an error nobody read, and every time it kept running and kept looking
healthy:

| what failed | how it presented |
|---|---|
| `xTaskCreate` could not get 8 KB | no beacons, no announcements, no clock -- everything else fine |
| `st7789_flush`'s return was discarded by the board's `display_flush` | a panel that never received a byte looked identical to one that did: LVGL renders, the flush callback runs, the UART framedump is **perfect**, and the glass stays dark with nothing in the log |
| `esp_get_free_heap_size()` on a board with PSRAM | the heap-floor alarm can never fire again, because free is permanently ~8 MB -- see "PSRAM does not mean more memory" |
| `httpd_start` could not get 5,120 in one piece | served nothing on the LAN while gossiping happily over ESP-NOW and BLE |
| `nimble_port_freertos_init` could not get 5,120 either | BLE never came up, so `relay_task` (which waits on `on_sync()`) stayed parked forever, taking the OTA self-test and the `cmd:update` answer with it |
| a block of `sdkconfig.defaults` vanished in a repo move | nothing at all, for months, until 2,912 bytes of new static RAM ended it |

The rule generalises past `xTaskCreate`: **anything that allocates can fail
here, and eventually will. Check it, and keep checking after boot** -- three of
those four were still true an hour later, and only one of them was visible in a
boot log anybody was reading.

The evidence for the BLE one had been printing all along. The heartbeat says
`relay=<loops>` precisely so a parked task is visible, and it had been sitting
at `relay=0` for a long time. **A number in a log line is not an alarm.**

So `common/xprs_health/` keeps a register of what a board is supposed to have,
declared *before* anything starts -- a part registered only when it succeeds can
never be reported missing -- and names whatever is absent at `ESP_LOGE`, at the
end of boot and from the heartbeat thereafter. `xh_all_ok()` is also the
verdict the OTA rollback self-test consumes, so "healthy enough to keep this
firmware" and "healthy enough to stop complaining" are one function and cannot
drift apart.

It also turns the tables in this file into a check. `xh_heap_floor()` shouts
when free heap is under what the board is documented to boot with -- the thing
that would have caught the missing `sdkconfig` block on the first boot instead
of months later. It is edge-triggered and runs periodically, because **a
subsystem that stops working does not stop existing**: proved on the bench with
a deliberately over-committed build, where the roster still read `http api+`
while every request went unanswered, and the floor check was the only thing
that said anything (`free=2412 largest=1728, expected at least 4000`). The
handle was valid the whole time. The handlers just could not allocate.

### Register a callback before you start the thing that fires it

`ble_hs_cfg.sync_cb` was assigned at the end of `app_main`, which was harmless
only because the host was started at the end too. Moving the host earlier meant
it synced against an unset callback: BLE came up and `on_sync` never ran. The
controller is ready immediately, so this race always loses -- there is no window
in which "start it, then wire it up" works on this chip.

### The boot order is the allocator

Whoever starts last gets the fragments. This file already says to claim big task
stacks early; extend it, because the two worst cases were not `xTaskCreate` calls
anyone can see in this repo:

- `httpd_start` wants 5,120 bytes in one piece and found 2,816
- `nimble_port_freertos_init` wants 5,120 and found 3,584

Both are library calls that create a task internally with a fixed stack. **If a
component starts a task, its position in `app_main` is a memory decision**, and
the fix in both cases was to move the call ahead of WiFi, the card and the
bearers rather than to trim anything.

**It happened a third time, on the T-Deck, and this time by exactly zero
bytes.** `xprs_api_start()` was the last call in `xapp_run()` -- after WiFi, the
LAN and ESP-NOW bearers, the SX1262 and the index. httpd asks for a 6,144-byte
task stack. Measured internal heap across that boot:

| after | internal free | largest block |
|---|---|---|
| before wifi | 174,824 | 110,592 |
| after wifi | 101,180 | 38,912 |
| at the old call site | **6,459** | **6,144** |

Asked for 6,144 against a largest block of 6,144 and lost, every boot, and the
station served nothing on the LAN while gossiping happily over ESP-NOW -- the
same presentation as the `httpd_start` row above. Hoisted to immediately after
`wifi_up()` it sees a 40,960-byte block. This affects **every board that uses
the shared `xapp_run()`**, not just the one it was found on.

Starting httpd before the interface has an address is safe and deliberate: it
binds a socket, it does not need a route, and `xprs_api_start()` keeps a
POINTER to its config, so the index handle `idx_task` fills in later is picked
up live and a request that beats it gets an honest 404.

### Freeing memory does not fix an over-committed board -- it moves the victim

Halving the LVGL pool handed back 16 KB and made the end-of-boot free number
*worse*. That is not a paradox: `rns_tcp` and `gatt_mesh` had been failing
quietly, and with room to succeed they took theirs. Every reclaim was absorbed
by whatever had been losing before it, and three rounds of this bought nothing.

**Judge by min-ever, and by whether every subsystem actually started -- never by
"free heap went up".**

What ended it was writing the budget down and subtracting:

*T-Dongle-S3, 2026-08-21, everything running:*

| subsystem | cost |
|---|---|
| WiFi + APRS-IS iGate | 55.7 KB |
| NimBLE + controller | 59.3 KB |
| SD card (3 open files) | 32.0 KB |
| Reticulum hub link | 12.7 KB |
| LAN + ESP-NOW bearers | 10.5 KB |
| HTTP server + its response buffer | 11.0 KB |
| LVGL pool | 16.0 KB |

against about 208 KB at boot -- roughly 12 KB more than exists. The answer was
not another reshuffle, it was **giving a feature up**: the hub link is off on
this board (`-DRNS_HUB_LINK` to restore it, and expect to lose something else).
When the arithmetic does not close, stop moving memory and decide what the board
is for.

### A boot-trace delta says what a subsystem cost, not what stopping it returns

The trace shows `heap after hotspot: 30,448`, down from 139,440, so the SoftAP
and its DHCP server and netif look like ~109 KB of recoverable memory. Dropping
to `WIFI_MODE_STA` returns **3,312 bytes** of it (7,628 -> 10,940, measured).
The rest was claimed when the interface was created and a mode change does not
give it back; that would mean tearing the netif down. Do not budget for memory
you have only seen disappear.

### Defaults are sized for a JSON request, not for bulk over flash

`httpd_config_t.recv_wait_timeout` is five seconds. A firmware push is over a
megabyte arriving while the same task erases flash -- and an erase stops the
cache for **both** cores. Two pushes died mid-transfer with `recv=-3` at 44 KB
and 403 KB while every `esp_ota_write` returned `ESP_OK`, which reads as a
storage fault and is a socket one. Both boards now use 30 s.

The same shape appears wherever a default assumes a small, quick transaction:
check it before blaming the thing that looks broken.

### "Quiesce" means hand resources back, not pause writes

The first version of `ota_quiesce()` paused the index writer and nothing else,
so a 1.4 MB transfer competed with the hub link, ESP-NOW and the bearers for
lwip buffers; the window shut and never reopened. Standing the station down --
`rns_tcp_pause()` giving up its socket and both TCP windows, `xprsnow_stop()`
its queues -- is what made the transfer complete, in 34 seconds. Resume on the
failure path too: a refused image must not cost the station its radios.

### A component that spawns a task must be asked before it dies

`xprsindex_open()` creates the store's writer task and hands it the store
pointer. `xprsindex_close()` freed that pointer -- and for months nothing
noticed, because nothing ever closed a store on a running board. The day the
M5Stack gained "Wipe archive" (close, delete the files, reopen), the first
wipe was a PANIC: the writer woke on its 2 s drain timer and walked freed
memory.

The rule: **any close/deinit of a struct that a task holds needs a shutdown
handshake, not a free.** The store now does it in three lines each side --
`close()` raises `st->closing` and waits; the writer task checks the flag at
the top of every drain cycle, marks `st->writer_gone`, and `vTaskDelete(NULL)`s
itself; only then does `close()` free. The wait is bounded (three drain
periods) so a wedged writer cannot hang the caller forever.

Two things made this cheap to find instead of a mystery:

- **The rotating log said PANIC in its first line.** `/log.txt` (and
  `/api/log`) begin every boot with the reset reason, and the heartbeat
  showed the exact second the wipe ran. One curl, no serial, no guessing --
  this is what the log was built for, and it paid for itself on its third
  day.
- The wipe runs on `idx_task`, the one task allowed near the storage, so the
  failure was a clean use-after-free rather than the FatFs cross-task
  corruption of the section above -- one bug, not two.

And an operator's note for the scripted kind: the serial debug keys
(`U`/`D`/`K` on the Settings panel) keep their focus and selection between
test sessions. Two "identical" key sequences an hour apart landed on
different rows -- one of them on Restart. Back out to a known state (send
more `U`s than the list has rows) before navigating by count, or use the
purpose-made key (`W` = wipe) instead of driving the menu.

### Task stacks are heap, and these are the measured floors

Every stack on this board comes out of the same ~15 KB, so they get trimmed --
and trimming them past what the task actually does turns a memory problem into a
reboot. Measured by overflowing them:

| Task | Floor | What costs the stack |
|---|---|---|
| `xprsidx_wr` | 4096 | FATFS and the SDMMC driver; 3072 overflowed under a burst |
| `xprslan` | 5120 | two SHA-256 derivations per datagram, a BLE re-air and a log line; 4096 overflowed |
| `aprsis` | 6144 | line parsing, DNS, socket; 4096 overflowed |
| `heartbeat` | 3072 | `ESP_LOG` with ten arguments is almost all of it; 2048 overflowed |

**`ESP_LOG` is the most stack-hungry thing a small task does.** A diagnostic
that crashes the board is worse than no diagnostic, and this one did, twice.

A stack overflow here presents as a **reboot loop of a few tens of seconds** --
which from the network looks exactly like a station that answers, then stops,
then answers again. Check the console for `***ERROR*** A stack overflow in task`
before believing anything about the network.

## PSRAM does not mean more memory

*T-Deck (N16R8, 8 MB octal PSRAM), measured 2026-08-21 on X3R8XX. Read this
before turning `CONFIG_SPIRAM` on anywhere.*

PSRAM adds eight megabytes of memory that **a task stack, a DMA buffer, or
anything touched while the flash cache is disabled can never live in**. Turning
it on also costs internal RAM. The first attempt on this board found 8 MB, passed
its memory test, and left the station **worse**:

```
esp_psram: Found 8MB PSRAM device / Speed: 80MHz / SPI SRAM memory test OK
xprs: heap before wifi: 8,495,712
E xprs_ui: draw buffer: 4480 bytes refused (free 8348892, largest DMA 64)
           -- the panel stays dark
E xprs:    display init failed - running headless
E xprs_api: API failed to start: ESP_ERR_HTTPD_TASK
```

Eight megabytes free and the panel could not have four kilobytes of it. This is
"freeing memory does not fix an over-committed board" arriving from a new
direction: what matters is not how much memory is free, it is whether the right
**kind** is free, and whether every subsystem actually started.

### The controlled A/B, which is the only honest way to judge this

Same commit, `CONFIG_SPIRAM` the only variable. Internal free heap by stage:

| | PSRAM off | on, cache 32 | on, cache 16 |
|---|---|---|---|
| before wifi | 180,528 | 174,824 | 174,824 |
| after wifi | 133,952 | 73,916 | **101,180** |
| after api | 122,316 | 62,396 | **89,712** |
| after hotspot | 31,116 | 4,272 | **25,580** |
| largest block | 21,504 | 1,920 | **17,408** |
| heap floor | silent | **FIRING** | silent |
| subsystems | all up | **UI + DNS tasks died** | all up |

Final cost: **5,536 bytes of internal RAM for 8,357,432 bytes of external.**
Worth it -- but only with both settings below, and the middle column is what
happens if you miss the second one.

### Two settings that are not optional, and are not obvious

**1. `SPIRAM_USE_CAPS_ALLOC`, not `SPIRAM_USE_MALLOC`.** Buried in
`esp_wifi/Kconfig`:

```
config ESP_WIFI_DYNAMIC_TX_BUFFER
    depends on !SPIRAM_USE_MALLOC
```

Backing ordinary `malloc()` with PSRAM therefore **forces WiFi onto static TX
buffers** -- 16 of them, ~1.6 KB each, claimed up front in internal RAM and never
released. WiFi's internal cost went 46,576 -> 123,804 bytes. `CAPS_ALLOC` makes
PSRAM reachable only through `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`, which
also means nothing lands there by accident, and
`CONFIG_FATFS_ALLOC_PREFER_EXTRAM` still applies (it depends on either mode), so
the FatFs 4 KB-per-open-file sector caches still move out -- which was the actual
"we had to turn the SD card off for the OTA to fit" complaint.

**2. `ESP_WIFI_CACHE_TX_BUFFER_NUM=16`.** This queue only exists when PSRAM does
(`depends on SPIRAM`) and defaults to **32**, each entry copying an uplayer
packet. That default alone is ~27 KB of internal RAM nobody asked for, and it is
the difference between the middle and right columns above -- at 32 this board
loses its screen. 16 is the Kconfig floor.

Also: `SPIRAM_IGNORE_NOTFOUND=y` is the boot-loop insurance -- without it, "PSRAM
configured but not detected" is a startup panic indistinguishable from a bad
flash. Keep `SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` off; this firmware writes flash
(OTA, coredump), which disables the cache on **both** cores. And 80 MHz, not
120: IDF's own help calls octal-at-120 experimental and warns it "will crash
randomly" after a ~20 degree swing.

`models/tdeck/firmware/sdkconfig.defaults` carries every one of these with its
measurement written next to it. Copy that block, not a blog post.

### PSRAM silently disables the heap alarm -- fix the instrument first

`esp_get_free_heap_size()` counts PSRAM. On a board that has it:

- `xh_heap_floor()` can never fire again, because free is permanently ~8 MB.
  That is the check this file calls "the thing that would have caught the
  missing sdkconfig block on the first boot instead of months later".
- the `alive` heartbeat reported `heap=8367348` while the HTTP server and the
  index writer were **both** failing to get a task stack.
- `/api/diag` reported `free` and `min_ever` as totals while `largest` was
  internal -- three numbers about two different memories in one JSON object.

All three now measure `MALLOC_CAP_INTERNAL`, with PSRAM reported separately and
labelled (`heap_mark()` in `xprs_app.c`, `xh_heap_floor()`, the API handlers).
Identical output on a board without PSRAM. **Nothing about the configuration
above was diagnosable until this was fixed** -- every round before it was
guesswork. If you add PSRAM to another board, check its instruments before you
tune anything.

### Recovery, so a bad PSRAM config is not a brick

Octal PSRAM claims GPIO 33-37; check the board's pin map before enabling it (the
T-Deck uses 0,1,2,3,8-13,15-18,38-42,45 -- clear). If the config is wrong the
symptom is a boot loop that looks exactly like a bad flash. On the T-Deck GPIO 0
is the trackball click **and** the boot strapping pin, so holding the trackball
down through a reset drops into ROM download mode, and `platformio.ini` already
pins `--no-stub`, which is what that mode needs.

### The one thing PSRAM is genuinely good for: moving `.bss` out of DRAM

Once PSRAM is on and behaving, its real payoff is not "eight more megabytes of
heap" -- almost nothing may live there. It is that **large CPU-only static
buffers can stop costing internal DRAM.** Measured on X3R8XX, 2026-08-21, this
moved 103,496 bytes and took the internal heap at the tightest point of the
boot from 3,492 bytes (largest block 1,600) to 70,940 (largest 31,744):

1. **LVGL's pool.** LVGL's TLSF heap is a plain array in `.bss`, sized by
   `CONFIG_LV_MEM_SIZE_KILOBYTES` and spent whether the UI uses it or not --
   50,249 B on this board. `CONFIG_LV_MEM_CUSTOM=y` plus
   `common/tinylv_mem/`, a PSRAM-preferring allocator, moves it. The board
   `CMakeLists.txt` `-D`s `LV_MEM_CUSTOM_INCLUDE` / `_ALLOC` / `_FREE` /
   `_REALLOC` at the LVGL component; they are `#ifndef`-guarded in
   `lv_conf_internal.h`, so the managed component is never patched.
2. **Our own big statics.** `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`
   plus `XPRS_PSRAM_BSS` (`common/xprs_common/include/xprs_psram.h`) on
   chat rings, device tables, statistics buckets and table-row scratch.

**What may NOT carry the attribute**, because PSRAM is reached through the
cache and is unreachable whenever the cache is off:

- anything touched from an ISR;
- anything touched by DMA -- the LVGL draw buffer and SPI TX buffers stay
  internal, allocated with `MALLOC_CAP_DMA`;
- anything read or written while flash is being written (NVS, OTA).

The log ring is the judgement call in this tree: it is filled from a `vprintf`
hook, so it was left internal. Six kilobytes is not worth a rare crash.

### Trimming flash does not help a board that is short of DRAM

This is worth stating plainly because it cost a day to learn twice. On the
T-Deck, 171,308 bytes of flash were removed in one session -- LVGL widgets,
the mbedTLS/WPA3 tail, a gzipped hotspot page -- and **the board could not
feel any of it.** Flash sat at 63 %, and had sat at 71 % before. Nothing about
the station changed until the DRAM moved.

Worse, one of those flash trims *crashed the board*. Applied on its own, the
mbedTLS config trim produced, on every boot:

```
W wifi: alloc eb len=752 type=4 fail
Guru Meditation Error: Core 0 panic'ed (LoadProhibited)
  ieee80211_hostap_attach <- wifi_softap_start <- _do_wifi_start
```

Nothing was wrong with the crypto -- secp256k1 still derived the right npub.
The station was living on 3,492 bytes of internal heap with a largest block of
1,600, and any change to the layout was a coin toss on the softAP's 752-byte
beacon allocation. **The same config, re-applied after the DRAM had been
freed, boots clean every time.**

The rule: when a board is that close to the floor, a build that succeeds
proves nothing and a build that crashes is not evidence the change was wrong.
Fix the floor first, then re-run the experiment.

### The FAT descriptor pool: count handles, not subsystems

`CONFIG_SDCARD_MAX_FILES` was 3 on the T-Deck, chosen against a model of "one
handle per subsystem — its index, its message store and its log". That model is
wrong. **The index alone holds three open by construction:**

| handle | closed only on |
|---|---|
| `xprsindex` `st->active_fp` — the current `seg_NNN` | a segment roll |
| `xprsindex` `st->tail_fp` — the current type tail | a type change, or a query |
| `xprs_app` `s_logfile` — `/idx/log/cur.txt` | 64 KB rotation |

So the pool was full at rest, and every transient open failed. What that looked
like:

```
E (17716) vfs_fat: open: no free file descriptors     <- twice, same millisecond
E (600477) vfs_fat: open: no free file descriptors    <- again, ten minutes in
```

Two at one timestamp is the signature of a single site doing an `r+b` then
`w+b` fallback — `xi_zone_write`, reached from `xi_sync_card` **while both index
handles are still held**. The 600 s one is `xst_stats_save` on its timer
(`last_stats_save_s` starts at 0, so it first fires at exactly `now_s == 600`).

**The damage was invisible.** `xi_zone_write`, `xst_stats_save`, `xi_decl_save`,
`xi_reg_save` and the eviction carry-forward all return `void` and logged
nothing of their own, so the zone map, the statistics rings, the declared
mailboxes, the regulars list and §36.11 mail retention had *all* stopped
persisting with `vfs_fat`'s line as the only trace. They now log on a failed
open (`XI_LOGE`). If you add a writer to this store, give it a failure line —
a `void` save function that cannot report is how this hid.

Two lessons, and the second is the general one:

1. Size the pool by **counting resident handles**, then add one per concurrent
   transient (`xi_zone_write`, a query's `read_fp`, an HTTP `/api/log` reader).
   Six on the T-Deck.
2. **Price it by measuring, not by reasoning.** Three extra ~4 KB `FIL`s reads
   like 12 KB of internal DRAM, and it is not: `CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y`
   puts them in PSRAM. Measured on X3R8XX, `heap after hotspot` went 70,940 →
   71,100 (unchanged) while PSRAM went 8,263,228 → 8,252,152. On a board with
   no PSRAM the 4 KB is real and internal.
3. **Where the 4 KB is real, give a handle back instead of buying one.** The
   T-Dongle cannot afford a bigger pool -- `CONFIG_SDCARD_MAX_FILES=3` is what
   fits beside WiFi -- and with LFN fixed it hit the same wall the moment its
   opens began to succeed: `zone map /sdcard/xprs/zone.idx not written: Too
   many open files in system`. Two of the store's three handles are only
   CACHES: `read_fp` saves a re-open on the next query and `tail_fp` is
   reopened on the next append of that type. `xi_fopen_pressed()` retries an
   auxiliary open after closing them, so the write that matters wins the
   descriptor and pays one `fopen` later. A cache that cannot be dropped under
   pressure is not a cache.

### Flashing the T-Deck: split the write

The T-Deck talks over the ESP32-S3's native USB-serial-JTAG, and a single
write of a ~1.4 MB image dies part-way through often enough to be a real
hazard:

```
A serial exception error occurred: Could not configure port: (5, 'Input/output error')
```

That leaves an **invalid image**, and with a single `factory` partition there
is nothing to fall back to: the board reboot-loops in the bootloader, which
makes the USB device churn and the next attempt harder. Recovery is
`erase_region` followed by a chunked write.

Use `tools/flash-chunked.sh <port> <image.bin> <offset>`, which writes 256 KB
at a time, each piece its own esptool invocation with its own reset and up to
four retries. The offset is required and comes from the board's
`partitions.csv` (the T-Deck app now starts at `0x20000`, `ota_0`). The script
accepts `0x0` (bootloader) and `0x8000` (partition table) so a board can be
moved to a new layout over the same link, and refuses anything else below
`0x10000`, which is nvs/otadata. Moving a board from the single-`factory`
layout to OTA slots needs all three writes -- bootloader, table, app -- and
an `erase_region 0xF000 0x2000` so stale otadata does not point at a slot
that is not there. A board that did not get the table write keeps its old
layout and reports `"running":"factory"` in `/api/diag`; it cannot take an
OTA until this is done by cable. Also: do not raise the baud on this link -- at 460800 esptool's
`Changing baud rate` step is itself a common failure point.

### A board without Bluetooth must still compile

`CONFIG_BT_ENABLED=n` does not merely disable the radio: ESP-IDF's `bt`
component then registers with an **empty `INCLUDE_DIRS`**, so *no* BLE header
resolves — not `nimble/nimble_port.h`, and not `esp_bt.h` either, which is why
selecting the tinynimble backend does not rescue it.

XPRS treats BLE as optional at runtime via the board descriptor's `bool ble`
(`xprs_app.h`), but a runtime flag cannot save a build. The guard belongs in
`xprs_bearer_ble/xprsble.c`, which is now `#if CONFIG_BT_ENABLED` with a stub
arm returning `ESP_ERR_NOT_SUPPORTED` / `false` / `0` / `-1`. `xprsble.h`
includes only `<stdint.h>`, `<stdbool.h>` and `"esp_err.h"`, so callers need no
`#ifdef` at all — `xprsble_is_active()` answering false is already the path they
take. `tinynimble` drops `tn_port_esp.c` under the same condition and keeps the
IDF-free `tn_hci.c`. Cost on the M5Stack: `libxprs_bearer_ble.a` links as
**27 bytes**.

The M5Stack is not merely unconfigured, it is incapable — the original ESP32
defines `SOC_BLE_SUPPORTED` but **not** `SOC_BLE_50_SUPPORTED`, so it has only
legacy 31-byte advertising and an XPRS frame does not fit.

**None of `models/*/firmware` is in `tools/build.sh`** — it covers only the
seven `multiboard` targets. That is why m5stack-core stayed broken for ten
hours after a shared component gained a new REQUIRES. Adding the three board
projects to that script is outstanding.

### Touch, and the three rules it arrived with

The T-Deck's GT911 is polled -- no interrupt -- by LVGL's own indev timer,
which runs inside `xui_update()` on the UI task. That is the same task that
reads the keyboard, so the shared I2C bus, which has no mutex, still has one
reader. Keep it that way: never read the panel or the keys from anywhere else.

What a tap MEANS is decided by the app, not by LVGL. Clickable objects push a
small event (`XUI_EV_ROW 3`, `XUI_EV_BAR 2`, a swipe) into a ring and the app
drains it every tick, translating bar taps and swipes into the same
`XAPP_KEY_*` the trackball produces. No LVGL groups, no focus tree: the app's
`s_panel` / `s_sel[]` / `s_set_focus` stay the single source of truth, and a
board without a panel sees none of it.

Three rules that came out of building it:

1. **The bar is capability-driven, byte-for-byte.** `touch_read == NULL`
   means the M5Stack's legends are emitted by the exact code they always
   were. A touch board's slots name what a tap does. Diff the M5 framedump
   against its baseline after touching the bar code.
2. **Waking is all a sleeping screen's first input does.** Key, trackball or
   touch: it brings the panel back and is then dropped. A blind keypress
   cannot type into the composer or toggle a setting.
3. **"On battery" is a trend, not a voltage.** Six samples a minute apart;
   ≤ −15 mV is discharging, ≥ +15 charging. The screen blanks only when
   discharging, so a bench station never goes dark, and a station fresh off
   the charger keeps its screen for the first minute. That is the
   conservative error, and it is deliberate.

And the settings bug that every board carried: `ui_render()` excluded panel 6
from the one block that calls `xui_table_select()`, so the highlight never
moved, the table never scrolled, and the selection grew unbounded into
`settings_ok()`'s `default:`. One condition; all boards.

### Bring the panel up early, and give it something to say

The screen used to be the last thing `xapp_run()` started, on the reasoning
that everything it reads already exists by then. That is true and it was
still wrong: `st7789_init` raises the backlight over GRAM it never clears, so
the user got several seconds of undefined pixels and then a finished
dashboard, with nothing in between to say the station was working.

Moving `display_init` + `xui_init` to just before `wifi_up()` costs nothing
and pays twice. Measured on X3R8XX:

| | display last | display before WiFi |
|---|---|---|
| draw buffer | 15 rows (9,600 B) | **30 rows (19,200 B)** |
| splash on screen at | 3,417 ms | **2,437 ms** |

The draw buffer is adaptive -- it takes an eighth of the screen when it can
get one contiguous DMA block and halves down to a floor of eight rows when it
cannot. Early in the boot the heap is still whole, so it gets the full
eighth and every later frame flushes in half as many SPI slices. **Do not
move it ahead of BLE**: the controller wants contiguous internal DRAM, and
the note above about `BLE_INIT: Malloc failed` is what taking it away looks
like.

Three rules that came out of doing it:

1. **A splash that arrives after the boot is decoration; one that arrives
   during it is information.** Give it the real step names — the sequence
   already has them — and drain `stdin` for `'S'` while it is up, because
   `ui_task` is what normally answers a framedump request and it does not
   exist yet. Without that there is no way to photograph the splash *during*
   boot, and therefore no way to prove it was ever on screen.
2. **Vectors, not a bitmap.** Ten stroked shapes are 212 bytes of `.rodata`
   and fit any panel; the same mark as 320x240 RGB565 is 153,600 bytes and
   fits exactly one. `lv_line` does not copy its points — the array has to
   outlive the objects and be freed with them.
3. **Only log what you can measure.** LVGL's pool is a fixed array on a board
   without PSRAM (`lv_mem_monitor` reports it exactly) and PSRAM on a board
   with it (`lv_mem_monitor` is inert). Free PSRAM across the splash's
   lifetime is not a measure of the splash — the rest of the boot allocates
   tens of kilobytes in that window. Report the real figure where there is
   one and say nothing where there is not.

## Memory budget -- the T-Dongle-S3, which has no PSRAM

`CONFIG_SPIRAM` is **not** set on the T-Dongle, so everything comes out of
internal SRAM, and the app partition is nearly full. (The T-Deck does have it
now -- see the section above, and do not read the numbers here as that board's.)

*Measured 2026-08-21, hub link off:*

| | |
|---|---|
| App slot (`ota_0`/`ota_1`, two of them) | 2,097,152 B each |
| Current app | ~1,446,000 B |
| Free heap when httpd starts | 73,140, largest block 31,744 |
| Free heap at steady state | **14,304**, largest 4,096, min-ever 4,724 |

The two app slots are the OTA layout (XPRS.md 25.8); there is no `factory`
partition. The old single-slot figure of 1,966,080 B with ~105 KB spare no
longer applies, and neither does "~104 KB free after httpd" -- that was
measured when httpd started last, before the boot order changed.

Consequences that have already bitten:

- an 8 KB request-time `malloc()` in an HTTP handler fails outright -- 2-3 KB is
  the realistic ceiling
- the httpd task stack is trimmed to **5120 B** on this board, so a handler that
  puts a couple of 600-byte buffers on the stack can take the server down; build
  responses straight into the response buffer
- SQLite does not fit, which is why `xprs_index` is what it is

### One response buffer, and the day this page was ignored

The rule three sections up -- *one response buffer, claimed at boot and
shared* -- was written for the T-Dongle's own server and never applied to
`common/xprs_api`, which every other board uses. That file had grown eight
private statics: 4,592 bytes of internal DRAM held whether or not anything
ever called the endpoint, plus a `malloc` per request in `/api/coredump`
and two 600-byte buffers on the httpd stack. Then, "fixing" a per-request
`malloc` in `/api/diag`, another 1,112 bytes of private static went in --
on the M5Stack, the board with no PSRAM, while the stated goal was to
*reduce* its fragmentation.

Worse, the T-Dongle was made to `REQUIRE` the whole component so it could
reuse one handler. A linker takes objects, not functions, so that board --
the one this page records at 14,304 bytes free -- inherited every static
in the file for a function it could have been handed.

*Measured 2026-08-23, all three boards, same commit either side:*

| component `.bss`/`.data` | T-Deck | M5Stack | T-Dongle |
|---|---|---|---|
| `xprs_api`, before | 4,592 | 4,592 | 3,480 |
| `xprs_api`, after | 8 | 8 | **0** |
| `xprs_diag` internal, before | 2,537 | 2,537 | 2,537 |
| `xprs_diag` internal, after | 1,141 | 1,141 | 1,141 |
| net internal DRAM | **-3,932** | **-3,932** | **-4,876** |

The app boards' net includes the one 2,048-byte buffer now claimed at
boot; the dongle's is a straight saving because it already had one.

Three things did it, and they are the general shape of this mistake:

1. **The shared door became its own translation unit** (`xapi_send.c`),
   holding no statics and taking the caller's buffer. The dongle now pulls
   that object and nothing else -- check with
   `grep -o 'libxprs_api\.a([a-z_]*\.c\.o)' <map> | sort -u`.
2. **Every handler in `xprs_api.c` builds into the one buffer**, claimed
   before `httpd_start` and fatal to the server if it cannot be had. The
   history rows and log lines came off the httpd stack at the same time.
3. **Compose in place, never slice-to-slice.** `snprintf`ing one slice of
   a buffer into another is what `-Wrestrict` refuses, and the fix is not
   a second buffer: write the prefix, escape straight into the answer
   after it, close the string. That deleted three slices on its own.

### xprs_diag, on every board

`common/xprs_diag` costs **1,141 bytes** of internal `.bss` (one parked
ask, one wire, a 384-byte file block) plus 2,024 bytes of `.rtc_noinit` --
RTC slow memory, 8 KB on the S3 and on the classic ESP32, and nothing else
in this firmware uses any of it. Two generations live there so the words of
a boot that crashed stay readable while this boot writes; the first version
froze them into a kilobyte of DRAM instead, which is a duplicate of memory
the chip was already holding for free. The dongle, which keeps no log on
flash, adds a 1.8 KB RAM tail so `cmd:zlog` has something to serve. No
malloc on the hot path; the coredump summary (~220 B) is read on the init
stack once. The pump runs on idx_task / relay_task, the tasks that already
pay for `xauth_check`'s secp256k1.

### A board with no PSRAM must survive its own updates

The M5Stack could not be updated over the air. Every push died the same
way -- `curl: (52) Empty reply from server` -- and the board's own log,
once it could say why, named it:

```
xprssig: ecp muladd failed: -0x0010 OUT OF MEMORY (heap free 2756, largest 1088)
xauth:   command claiming X38364 does not verify -- discarded
xota_http: update refused: unsigned
```

A good signature, refused as unsigned. The WiFi driver takes its dynamic
buffers from the same internal heap everything else lives in, and the
defaults -- 32 receive and 32 transmit, about 1.6 KB each -- are sized for
a board with room to spare. This one has about 33 KB free once the hotspot
is up. A 1.4 MB push at full speed let the driver swallow the heap while
the update handler was still checking the signature, so the curve maths
had nothing to allocate.

Capped at 12 and 16, with the TCP window down from 5,760 to 2,880 so the
sender cannot outrun the board (`sdkconfig.defaults`). A push now takes
about forty seconds instead of thirty, and it finishes. The T-Deck keeps
the defaults: it has PSRAM.

**Two traps on the way.** The option is `CONFIG_ESP_WIFI_*` in ESP-IDF 5 --
`CONFIG_ESP32_WIFI_*` is a compatibility alias and setting *that* in
`sdkconfig.defaults` changes nothing. And the generated `sdkconfig.<env>`
is checked in, so it wins over `sdkconfig.defaults` for a board that has
been built before; change both, or delete the generated file.

And the refusal itself was a lie. "Does not verify" and "could not be
verified" are different facts, and the second one is worth asking again
about. `xprssig_last_result()` now reports which, `xauth_check` answers
`XAUTH_429` when the maths ran out of memory rather than the silence a
forgery gets, and the door replies `503 ... push again` instead of `403
this station takes updates only from its owner`. `tools/push_firmware.sh`
retries three times on that and on a dead socket.

## What the T-Dongle stores and speaks

- `xprs_msgstore` -- the APRS archives (`/sdcard/aprs/msg`, `.../beacon`),
  192-byte records, served by `/api/aprs` and `/api/beacons`.
- `xprs_index` -- every XPRS packet heard, verbatim, 320-byte records in
  segments, with a zone map for time ranges and a tail index per type. Serves
  `/api/xprs` and the GATT `xprs_query`. Section 36 of `XPRS.md` is enforced in
  the store: a packet with `d:` is held and never handed to a third party.
- `xprs_bearer_lan` -- XPRS as UDP broadcast on the LAN, port 4242, the same
  number XPRS answers on over TCP (`XPRS.md` section 24.4). See [lan.md](lan.md). Not
  Reticulum (that is UDP 42671, `xprs_lanwatch`, listen only) and not the
  internet.

## Scoped rooms (XPRS 13.11) on a station

The hotspot chat groups traffic into rooms by SCOPE, not by which bearer
happened to carry a packet: the Local room is the `scope:local` conversation,
Global is the unmarked default, and a `d:`-addressed message is a 1:1 room.
Sending in Local appends `scope:local` to the wire; the station's bearers
(ESP-NOW, LAN) are all local-class, so nothing else changes on this board.

Where the bearer DOES matter it is now recorded: `xprs_index` keeps a
one-byte bearer code per record (`xprsidx_bearer_t`, written into what was an
explicit pad byte -- the record stays 320 bytes and stores written before the
field read on untouched, reporting "unknown"). `xprsindex_add2()` takes the
code; the plain `add()` writes unknown. The HTTP API's `bearer` row field
comes from it.

The approved wires, verbatim from a live run (a Chat-wapp local message
archived on the M5Stack across the LAN):

```
t:message f:X16JK8 ts:2026-08-19_14:37:08 scope:local sig:<60> m:hello from the Chat wapp, locally
t:message f:X9WEB ts:2026-08-19_14:58:01 m:round two global
```

Three clients, one grammar: the Flutter Chat wapp writes them through
`hal_xprs_send` (host-validated, host-signed, scope rules per bearer), the
hotspot web page through `POST /api/xprs/send`, and the LCD's Chat panel
reads them out of the same archive with a house/pin/envelope icon for
local/global/direct.

**LoRa is the operator's call.** The spec (13.11.1) leaves whether a LoRa
link counts as a local bearer to the station owner -- a building mesh is
local, a forty-kilometre shot is not -- with NOT-local as the default. When a
LoRa-equipped board lands here, that choice belongs in config.ini
(`[lora] local = yes/no`, default no) next to the other switches, and the
relay/room logic reads it rather than hard-coding either answer.

## Scripts (Wrench) -- what to put in them, and what never to

`common/xprs_wrench/` is the vendored Wrench VM (7.2.2, MIT, two files, do
not edit them -- every choice is a `-D` in its CMakeLists). `common/xprs_script/`
is the station-facing host: the task, its PSRAM pool, its natives, and the
signed-bundle loader. `common/xprs_script/xs_bundle.h` documents the container.

### Be clear about what this buys, because it is not space

Measured on the T-Deck, linked into the real image:

| | |
|---|---|
| flash added | **+20,956 B** (`libxprs_wrench.a` 18,148, `libxprs_script.a` 1,955) |
| internal `.bss`+`.data` added | +456 B |
| internal RAM added | **-12 KB**, permanently, for the script task's stack |
| external RAM | -256 KB of 8 MB (the capped pool) |
| a whole script bundle | **458 bytes** |

**Scripting makes the image BIGGER, not smaller.** The panel surface it could
replace measures ~12.6 KB of `.text`; the VM costs 21 KB. Anyone reaching for
Wrench to fix a flash or heap problem is about to make it worse, and this file
has a whole section on why that mistake keeps happening.

What it does buy, and it is worth having:

- **a change ships as a signed bundle of a few hundred bytes instead of a
  1.37 MB image and a reboot.** That is the whole case.
- per-deployment policy without per-deployment firmware branches.
- a third party can add behaviour without a fork.

So: **use a script when the thing changes more often than the firmware
does.** Use C when it does not.

### What belongs in a script

Rare-firing, small-data, policy-shaped work: UI panel content, relay and
digipeat rules, iGate filters, alert triggers, beacon composition, reactions to
a new XPRS packet type.

### What must never be a script

Bearers, the XPRS wire codec, crypto, OTA, and **anything on a receive path or
with a deadline**. Reticulum in particular: `libxprs_rns.a` links at 951
bytes and its real cost is ~12.7 KB of *heap* (a socket and two lwIP windows)
that a script version would pay identically. There is nothing to win there.

### The rules the host already enforces, which must not be relaxed

- **The VM runs on ONE core-1 task and is reached only through a queue.** Never
  call it from a bearer, a receive path or the NimBLE host task -- that is the
  bug the top of this file is about. Extend the existing
  `seen_note()` -> `idx_task` parking pattern instead.
- **All script memory is a capped PSRAM pool** via `wr_setGlobalAllocator()`, so
  a leaking script takes itself down and not the WiFi driver's keepalive frame.
  There is deliberately **no fallback to the internal heap**: with no PSRAM the
  host refuses to start and says so.
- **Signed bytecode only, verified before the VM sees a byte.** Wrench's own
  check is a CRC -- trivially forged -- and the VM does not bounds-check
  bytecode, so malformed bytecode is a memory-safety problem, not a script
  error. The scheme mirrors `xprs_ota`: `xprsscr1 <board> <id> <version> <len>
  <sha256>`, bound to the board so a bundle cannot travel and an old approval
  cannot be replayed.
- **A separate publisher key.** Config `scriptkey`, falling back to `fwkey`. Two
  keys so "may publish panels for this station" can be delegated without also
  delegating "may reflash the roof". With neither set nothing verifies -- a
  station that has not been told whom to trust runs nobody's code.
- **`xh_expect("scripts", false)` -- the `false` is load-bearing.**
  `xh_all_ok()` is the OTA rollback verdict. If a script's failure could make it
  false, anyone who can publish a script could roll back the firmware. A refused
  bundle must leave the roster reading `station up: scripts- ...`, not
  `STATION DEGRADED`.
- **Wrench is not a sandbox.** A script cannot reach anything it has not been
  handed a native for, so **the native list IS the attack surface** and is the
  only thing worth arguing about. No signing keys, no raw filesystem, no
  sockets, no GPIO/SPI/I2C, no LVGL object handles, no `esp_restart`, and config
  access namespaced away from `nsec`/`pass`/`fwkey`/`own*`.

### Measured behaviour, so nobody re-derives it

- **Script recursion does NOT consume C stack.** 8,728 bytes left at depths 1,
  8, 32, 64, 128 and 200 alike, and unchanged after two minutes of an infinite
  loop. Wrench recurses on its own value stack, in the pool. Upstream issue #54
  (an instruction fetch from `0x80` on ESP32-C3) did not reproduce.
- **`WRENCH_PROTECT_STACK_FROM_OVERFLOW` works.** A 48-entry value stack
  overflowed at depth 32 and returned `WR_ERR_stack_overflow` cleanly -- error
  returned, task alive, station still answering. Pass the size explicitly to
  `wr_newState()`; a build-time define silently failed to propagate once
  (CMakeLists said 256, the compile used 48, stale CMake cache).
- **An infinite script does not take the station off the air**: 90 of 90 pings,
  0% loss, while `while(1){}` ran on core 1. The script task is priority 2,
  below `ui` (4) and `idx` (3), yields on a time slice and feeds the watchdog.
- Slice timings measured with `esp_timer` are **wall clock, not CPU time** --
  they span preemption by higher-priority tasks on the same core. A 55 ms "slice"
  is mostly the script being descheduled, which is the design working.

### The toolchain

Scripts are compiled **on a desk**, never on the station
(`WRENCH_WITHOUT_COMPILER` -- upstream's own header calls this required on
embedded). `tools/build_wrenchc.sh` builds `tools/wrenchc` from the same
vendored source the firmware runs, so the compiler and the VM cannot be
different versions. `tools/mkbundle.py build` packs and prints the line to sign;
sign it with `tools/sign_firmware.dart` (production) and stamp it back with
`mkbundle.py stamp`. Bundles live in the `script_a`/`script_b` partitions, A/B
so a bad one can be reverted.

Two host tests keep the format honest and must stay green:
`common/xprs_script/test/test_bundle_host.sh` (the packer and the device parser
agree; malformed containers are refused) and its verification half (a signed
bundle verifies; unsigned, tampered, replayed, cross-board and wrong-key ones do
not), both using the firmware's own signing and hashing code.

### The refusal path is the ordinary path -- make it as solid as the happy one

An erased partition, no key configured, a signature that does not check: these
are what a station sees most days, not forgeries. The first bench build
reboot-looped on `LoadProhibited` because with no bundle loaded the VM context
is NULL and nothing guarded it. Two more of the same shape followed: the loader
read `scriptkey` before `xcfg_init()` had run (fixed with a weak
`xs_app_ready()` hook called once the key is seeded -- the same
claim-the-stack-early-then-block-on-a-flag shape as `relay_task`), and
`scriptkey` was added to `xprs_config`'s INI-name map but not to the `s_cfg[]`
known-key array that `xcfg_get`/`xcfg_set` actually use, so it could never be
read or written. **`xprs_config` has two tables; a new key needs both.**

## Validating on the device

- **Opening `/dev/ttyACM0` reboots the board.** `monitor-capture.sh` asserts DTR
  whether or not `-r` is passed, and the board needs ~28 s afterwards before it
  is on the network again. A probe sent during a capture hits a rebooting
  device, and the result means nothing. The M5Stack's CP2104 (`/dev/ttyUSB0`)
  does the same on every `open()` regardless of pyserial's pre-open DTR/RTS
  settings -- open once, ride out the boot (~45 s to WiFi), keep the port open
  for the whole session, and drive everything else over the network.
- So **measure over the network** -- ping, curl, UDP -- with no serial open. Use
  serial only for facts that are only visible at boot: heap sizes, "HTTP server
  started", WiFi disconnect reasons.
- Reachability is the honest metric for anything touching the radio. One ping a
  second for three minutes, reported as *n* of *m*, is what turned a week of
  plausible theories into the table above.
- Change one thing at a time and keep the baseline. Four radio settings were
  changed on a wrong hypothesis before an A/B against `FEATURE_SDCARD=0` showed
  where the problem actually was.
- **A screenshot proves the FRAMEBUFFER, not the panel.** `xui_framedump()` (the
  `S` serial key, decoded by `tools/scripts/framedump.py`) emits its base64
  *inside* `lcd_flush_cb`, immediately before `st7789_flush()`. So a perfect
  320x240 image off the UART says LVGL rendered and the flush callback ran; it
  says nothing about what the glass received. When a panel is dark and the
  screenshot is fine, the fault is below that line -- SPI, chip select,
  backlight, or panel power. `TDECK_PANEL_SELFTEST` in
  `models/tdeck/firmware/src/board.h` (default 0) paints red then green straight
  from `st7789_fill_color()`, below LVGL, which is the bisect that separates the
  two in one boot.
- **"Every subsystem is up" does not include the screen.** `xprs_health`'s
  roster covers what has been `xh_expect()`ed, and the UI task is not in it, so
  `station up: ...` is silent about a dark panel. Do not read it as one.
- **A failed upload can leave a board that looks broken.** A `pio run -t upload`
  that dies partway (the S3's native USB-JTAG re-enumerates, and the port can
  come back as a DIFFERENT `/dev/ttyACM*` than it left on) leaves a half-written
  image: dark screen, or no USB device at all. Reflash before diagnosing
  anything. Confirm the board by MAC (`esptool.py chip_id`) rather than by port
  number, and remember `platformio.ini` pins ONE T-Deck's `upload_port` by id --
  the second T-Deck on the bench needs `--upload-port` given explicitly.

### Open

- SD-backed HTTP endpoints (`/api/xprs`, and the pre-existing `/api/beacons`)
  time out in longer runs even though httpd starts cleanly with ~104 KB free.
  Not diagnosed.
- Reachability has been measured at 178/182 and, an hour later on the same
  firmware, 0/70 with the STA failing to associate (reasons 202/205). Whether
  that is the access point or a late regression is unresolved -- re-measure
  before trusting a single run.

### Diagnostics over the air, rehearsed

With the dongle as gateway and a deck on ESP-NOW, consoles unread:
`tools/xprs_cmd.sh ... --cmd zdiag` answers in one frame; `--cmd zlog
zq=health` pages newest-first and closes with `200`; the seventh page in
an hour is `429`; the same signed wire twice re-airs the first code. A
`-DXDIAG_TEST_HOOKS` build's `cfg zpanic` reboots into a beacon carrying
`zc:`, and `--cmd zcore` plus `tools/xprs_bt.sh` names the line. `cfg
zhang` is the interrupt-watchdog shape: if a board does not come back from
it, the panic path itself is the bug, and that is what to chase.

## Known gaps / next steps

- Legacy T-Dongle firmware (`xprs_ble_hello`) knows nothing of `am:`/`?ACK`/
  `ENC1:`/0x4D -- fine as long as it's used for legacy-only deployments.
- GATT multi-parcel RX is unimplemented in the legacy firmware (single parcel
  only). rns_ble5 HAS a full MSP GATT server since M2 (`src/gatt_mesh.c`):
  FFE0/FFF1/FFF2, legacy connectable advert on ext-adv instance 1
  (NIMBLE_ROLE_PERIPHERAL=y, MAX_EXT_ADV_INSTANCES=2, MAX_CONNECTIONS=1),
  notify TX ring paced by NOTIFY_TX + 5 s in-flight watchdog, idle-central
  reaper, SD bulk spool at /sdcard/mesh/bulk with a RAM-FIRST index (the FAT
  VFS readdir deadlocks against concurrent SD writers -- never walk the dir
  outside boot), console `sendfile <to> <path>` / `transfers` / `recv`
  (base64 file upload over USB) / `advon|advoff` / `scankick` / `wifioff`.
  TRAPS: CONFIG_BT_CTRL_BLE_SCAN_DUPL must be n (the controller dedups scan
  reports BY ADDRESS -- fixed-address phones are reported once per boot and
  then never again); NimBLE may fail to route connection GAP events to the
  adv-instance callback -- register a ble_gap_event_listener instead; an ESP
  reset does NOT power-cycle a wedged SD card (physical replug only).
- Beacon carries the [pending_msgs][pending_bulk] trailer since M2; the
  have-digest bloom stays empty on the dongle (a carrier receives no 1:1s).
- Duplicate-delivery edge: SCF re-air more than ~60 min after the receiver
  already got the message can re-show it (phone content-dedup window) -- the
  `?ACK` purge covers the normal case.
- The old `xprs_mesh` component is the DISABLED ESP-WIFI-MESH bridge
  (`FEATURE_MESH=0`), unrelated to the BLE street mesh -- don't confuse the two.
