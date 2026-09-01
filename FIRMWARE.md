# Firmware, across the fleet

What the firmware in this tree does, board by board, and the behaviour that is
the same everywhere so it only has to be understood once. This is the document
to read before touching any board: the goal is that a person — or a program —
can learn how a station behaves and why, without reading ten `main.c` files.

It leans on the deeper docs rather than repeating them:
[`docs/device.md`](docs/device.md) (a station's obligations),
[`docs/esp32.md`](docs/esp32.md) (memory and scheduling),
[`docs/ble5.md`](docs/ble5.md) / [`docs/ble5-gatt.md`](docs/ble5-gatt.md) /
[`docs/ble5-nrf52.md`](docs/ble5-nrf52.md) (Bluetooth),
[`docs/espnow.md`](docs/espnow.md), [`docs/lan.md`](docs/lan.md),
[`docs/API.md`](docs/API.md), and the protocol itself in
[`../spec/XPRS.md`](../spec/XPRS.md). Each board's own `models/<board>/README.md`
and `board.yml` carry its pinout and catalogue entry.

---

## 1. How the firmware is organised

```
common/       the shared component library — every board draws from here
models/       one folder per board (pinout, docs, and sometimes its own project)
multiboard/   the PlatformIO project that builds the older ESP32 boards
tools/        flashing, monitoring, signing and probing
docs/         what is true across boards
```

**One station, written once, run many ways.** The parts that define a station
— the wire format, the relay decision, the signature, the bearers — live in
`common/` and are shared by every board. What differs per board is its radios,
its pins, and which of those shared parts it turns on.

There are three kinds of firmware in the tree:

| Kind | Boards | Where |
|---|---|---|
| **Own project** | `tdongle-s3`, `m5stack-core`, `tdeck`, `sensecap-p1-pro` | `models/<board>/firmware/` |
| **`multiboard` target** | `heltec-v1/v2/v3`, `kv4p`, `esp32c3-mini`, `epaper-1in54`, `generic` | `multiboard/` builds them |
| **Not an ESP32** | `sensecap-p1-pro` | Nordic nRF52840, Arduino/Adafruit core |

The shared code reaches an ESP-IDF board as an IDF **component** and reaches the
nRF52 board as a PlatformIO **library** (symlinked into `firmware/lib/`). The
same `.c` files compile both ways — see §7 for what that cost.

---

## 2. What every station does

Independently of its radios, a station:

- **Has an identity.** A callsign of the form `X1/X3/X4/X5` + characters
  derived from a secp256k1 key (spec §3). Stations are `X3`. The key is the
  station's; the callsign is derived from it, so a receiver can re-derive the
  callsign and know the two belong together — no trust-on-first-use.
- **Signs what it says** (spec §9.1), a 48-byte Schnorr signature carried as 60
  base85 characters in `sig:`. Receivers must still accept unsigned packets but
  must never present an unsigned `f:` as established.
- **Beacons who it is.** A `t:observation` naming the stations it hears
  directly (`hears:`, §10.6.3), and periodically a `t:identity` carrying the
  key behind the callsign (§9.3) so neighbours can verify everything after it.
- **Relays.** It repeats what it hears within a hop budget, appending itself to
  `via:` and never rewriting `f:` (spec §13). See §4.
- **Dates its packets.** `ts:` under a real clock, `epoch:<boots>.<uptime>`
  when it has none (§10.7), so a receiver can still order a clockless station's
  traffic.
- **Says whether it is well.** It declares the parts it should have before
  starting them and names anything that failed to come up, at boot and from its
  heartbeat (`common/xprs_health`). The same verdict gates an OTA's rollback
  self-test (§6).

The shared implementation is `common/xprs_app` (the ESP32 station) plus
`common/xprs_station`, `common/xprs_codec` (the wire), `common/xprs_bearer`
(the relay queue), `common/xprs_sig` / `common/xprs_id` (signatures). The
nRF52 board is not `xprs_app` — it is a smaller `main.cpp` that calls the same
`xprs_codec` and `xprs_bearer` directly (§9).

---

## 3. Bearers

A **bearer** is one medium a station carries XPRS over. A station bridges
between the bearers it has: what it hears on one it offers to the others, and
`xprs_bearer` decides whether it goes out.

| Bearer | Medium | Component | Boards |
|---|---|---|---|
| **BLE5** | Bluetooth 5 extended advertising | `xprs_bearer_ble` | any BLE5 chip |
| **LAN** | UDP on the local network | `xprs_bearer_lan` | any with WiFi |
| **ESP-NOW** | Espressif's connectionless 2.4 GHz | `xprs_bearer_now` | ESP32 family |
| **LoRa** | SX1276 / SX1262 sub-GHz | `xprs_bearer_lora` | LoRa boards |
| **VHF** | 2 m FM through an SA818, AFSK/packet | `xprs_sa818` | `kv4p` |
| **RNS** | Reticulum | `xprs_bearer_rns` | where enabled |

Two things about bearers are worth knowing up front:

- **BLE5 needs an extended advert.** An XPRS beacon is 112–173 bytes; the
  original ESP32 and other BLE 4.2 parts only do 31-byte legacy adverts, so
  they have **no BLE5 bearer** (`heltec-v1/v2`, `m5stack-core`). BLE 5 parts
  (all the S3 boards, the nRF52) do.
- **A bearer's task pumps the others.** On the ESP32 station the LAN bearer's
  task is what drives every bearer's re-air queue and beacon timer. A board
  with no WiFi (the nRF52) has no LAN bearer, so it must pump the queue itself
  — which it does from its own loop. Porting `xprs_app` to a WiFi-less board
  means pulling that pump out of the LAN bearer first (see the P1-Pro README).

---

## 4. Relaying — the behaviour to get right

A digipeater repeats what it hears so a station beyond the sender's range can
receive it. The rules are the spec's, enforced in `common/xprs_bearer`:

- **`via:`, never `f:`.** A relay appends its own callsign to `via:` and leaves
  the author alone. The hop count is the number of callsigns in `via:`.
- **A hop budget by packet type** (§13.1): `sos`/`warning` travel 9 relays,
  everything else 3. The limit is the type's, not a field's.
- **No loops** (§13.2): a station that finds its own callsign in `via:` does
  not relay, whatever the count says; and it drops a packet it has already
  relayed in the last few minutes, by the §5 identifier.
- **When to re-air** (§13.2.1): on a shared radio every station hears the same
  packet at the same instant and would collide re-airing it. So each waits a
  random moment and **cancels its own re-air if it hears someone else's relayed
  copy first**. "Relayed by *another*" is the load-bearing test: a non-empty
  `via:` is not enough — a station that wrongly appended itself to its own
  packet would look relayed and make everyone cancel. The test is whether
  `via:` names anybody who is not the author.
- **Cross-bearer vs same-medium.** `xb_offer()` is the bridge to *another*
  medium (hearing it here means it is already on this one — add nothing).
  `xb_digipeat()` is a repeat on the *same* medium it was heard on — hearing it
  here is the *reason* to repeat. Getting these two backwards makes one of them
  a silent no-op.

The identifier and the signature are both computed with `via:` removed, so
relaying changes neither: a station that already holds a message recognises the
repeat and does not show it twice.

---

## 5. Identity, keys and the clock

- **Key storage differs by chip.** ESP32 boards keep the key in NVS
  (`common/xprs_nostr`). The nRF52 keeps it in the internal LittleFS, generated
  once on first boot and derived into an `X3` callsign the same way.
- **The clock is a real problem on a headless node.** A station with no RTC and
  no NTP cannot judge the freshness of a command (§8) and refuses one it cannot
  date. The nRF52 station has no clock source, so it **learns the time from
  signed owner traffic**: each valid, signed command from an allow-listed owner
  carries a `ts:`, and the station moves its clock forward to the newest one
  (never backward, so a replayed old command cannot rewind it). This is enough
  to age commands and to stamp its own packets; it is documented as a deliberate
  weakening for a clockless node, closed by a real clock when one exists.

---

## 6. Remote firmware update

A station on a roof or a pole must update over the air or stay on the version
it was carried up with (spec §25.8). The design is the same on every board and
its whole premise is one sentence: **the station accepts an image, never a
source.**

**Two keys, on purpose:**

- The **publisher** key (pinned as `fwkey`) approves the *image*: a signature
  over `xprsfw1 <board> <version> <size> <sha256>`. This binds board, version,
  size and content together, so a build for one board cannot install on
  another and last version's approval cannot be replayed onto the next.
- An **owner** key (allow-listed as `own1..own4`) authorises *this station* to
  take it: a signed `t:command`, checked by `common/xprs_auth` — on the
  allow-list, signed, direct (never carried via a relay), inside a 300-second
  freshness window, and not a repeat. An `X3` callsign derives from its key, so
  this is airtight with no trust-on-first-use.

Nothing reaches flash before both verify. The old image stays put until the new
one proves it can come up and stay up; if it cannot, the old one is put back and
reports the failure — nobody climbs the ladder.

Two transports carry the bytes, and this is where the boards differ:

### 6.1 HTTP push (the WiFi boards)

`POST /api/update` (`common/xprs_ota/xota_http.c`) takes the image over plain
HTTP on the LAN — the signature makes the transport untrusted, so TLS would
only prove which server answered and cost heap the board does not have. The
station verifies the approval *before* the first byte reaches flash, streams the
rest into the spare OTA partition, and commits only on a full, matching image.
The ESP32's own bootloader does the slot swap and the rollback
(`otadata` + `ota_0`/`ota_1`, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).
`tools/push_firmware.sh` drives it. **Needs roughly 25 KB of free heap at the
instant the signature is checked** — which is the instant the image is arriving
at full speed — so a board tight on RAM (the M5Stack) takes a push over its own
AP rather than fetching, and a board with no OTA slot cannot host it at all
(`docs/device.md`, `docs/esp32.md`).

### 6.2 GATT push (the nRF52 P1-Pro)

The P1-Pro has no WiFi and no ESP-IDF OTA machinery, so the image arrives the
way everything else does on that chip — as XPRS packets — but over a **private
1:1 BLE GATT connection**, not the broadcast plane. This is the important part
and the one that was hard-won (§7):

- The owner's signed `cmd:update` names the version, size and sha256; the
  station answers `202` with a chunk count.
- The station **dials** a peer that serves the `tn_att` mesh channel
  (`FFE0`/`FFF1`/`FFF2`) and the image streams over the connection via
  **XBLOB** (`common/xprs_blob`): a binary manifest of per-block hashes, a
  windowed blast of raw 240-byte parcels, and a NEED-bitmap that re-requests
  exactly the missing or corrupt blocks. Measured: a 168 KB image in ~20 s
  at 2M PHY with 251-byte LL packets. The base85 `cmd:zfw` text lane remains
  as the fallback, and `cmd:zfwq` still names missing chunks there.
- There is **no second app slot** on this chip, so the firmware makes one: the
  flash above the application holds what runs, what is arriving, and a copy of
  what ran before. A RAM-resident copier (interrupts and SoftDevice off) swaps
  the staged image in and keeps the old one aside.
- The new image is on **probation**: it has two minutes with a working radio to
  prove itself; three boots without that proof — a hang or a crash loop — and
  the copier restores the previous image. A 60-second watchdog turns a hang
  into one of those boots.

`tools/push_firmware_p1.py --gatt <probe-port>` drives it through a
`tools/tinynimble_probe` acting as a transparent serial↔GATT bridge.
**Bench-validated end to end, 2026-08-31:** a 164 KB image delivered over the
connection, installed, booted, and kept, with the callsign and keys intact.

---

## 7. What the nRF52 port taught (the lessons)

The SenseCAP P1-Pro is the first board here that is not an ESP32, and getting a
station — and then an over-the-air update — onto it turned up rules that are
worth carrying to the next non-ESP32 board and, in a couple of cases, back to
the ESP32 boards.

**Bulk belongs on a connection, not on the broadcast plane.** The first attempt
pushed the image chunk-by-chunk as BLE *adverts*. That put the whole transfer on
the three advertising channels every device in the room shares, made every
listener derive a SHA-256 and drop each chunk, and depended on a gateway's
re-air queue. It was slow and it flooded the neighbours. A 1:1 GATT connection
runs on the other 37 channels, is heard only by the two ends, and delivers with
acknowledgement. This is exactly what `docs/ble5-gatt.md` argues, now measured:
the same image that stalled on adverts completed over a connection.

**On the nRF52, flash and the SoftDevice fight, and the app mediates.** Three
rules fell out and every one of them cost a debugging session:

1. **A filesystem write must happen with the SoftDevice down.** With it up, the
   core's LittleFS layer blocks on a completion semaphore that only a SoC event
   feeds, and that event is pumped on the same task — a deadlock. Worse, a
   half-finished write can lose the key. So the key, the config and the boot
   counter are all written before the SoftDevice starts (or after it is taken
   down), and the station reboots after a config change.
2. **A flash erase starves the radio.** Erasing many pages back-to-back holds
   the SoftDevice off the air long enough to drop a live BLE link (supervision
   timeout). The staging area is now erased one page at a time, as each chunk
   lands, spreading the stalls out so the link keeps its connection events.
3. **A flash wait must not re-enter the receive callback.** Waiting for a
   flash-done event by pumping the *whole* BLE event queue re-delivered the next
   GATT frame into the receive callback from inside the frame already being
   processed — corrupting the pump's static event buffer and hanging the board
   under a fast push. The wait now drains **SoC events only**
   (`tn_soc_pump`), never BLE events. General rule: **never pump an event
   source from inside one of its own callbacks.**

**The bootloader decides what "a valid app" is, and it is version-specific.**
The XIAO's UF2 bootloader (0.9.2) accepts whatever the copier writes only if the
bootloader settings page is left alone (its stored app-CRC is disabled). An
earlier copier erased that page and the board dropped to DFU. Read the actual
bootloader's rules before writing where it looks.

**The Arduino core's task stack is too small for crypto.** A secp256k1
signature is scalar multiplication plus wire-sized buffers and overflowed the
core's 4 KB `loop` stack silently. The station runs on its own 12 KB task.

**A transport that cannot say "busy" loses data invisibly.** The ESP32 port
sent ACL packets while only checking the VHCI pipe; HCI's contract is
Number-Of-Completed-Packets credit accounting, and without it a bulk blast
overflowed the controller's buffer pool and 99% of the frames vanished with
no error on either side. The credit counter in `tn_port_esp.c` is what makes
"busy" honest -- and every layer above it works only because of that.

**Backpressure beats blind pacing.** Streaming chunks faster than the link
drained overflowed the bridge's UART and lost most of them. A per-frame
acknowledgement — the receiver says "sent, send the next" — paced the sender
exactly to the link and made the transfer reliable, where a fixed rate either
crawled or lost data.

**A shared-code bug can hide behind a better clock.** The freshness check in
`common/xprs_auth` subtracted timestamps as unsigned values; a command even one
second *ahead* of the station's clock underflowed and read as ancient. It never
showed on the NTP-clocked ESP32 boards, whose commands arrive slightly in the
past — it only surfaced on the clockless P1 whose clock lags. Fixed in the
shared code, so every board benefits.

**Bench note, not firmware:** an ESP32 used as the GATT server kept getting
reverted by its *own* OTA rollback (it never marks a hand-flashed probe image
valid), and a screen-heavy board crash-loops under load. For the GATT server
role, use a spare board with no other firmware to fall back to (full-erase it),
or a T-Dongle rather than the T-Deck.

---

## 8. The boards

Status is from each `board.yml`: **shipping** (runs its intended firmware),
**legacy** (an older `multiboard` image, not recently retested), **planned**
(hardware chosen, firmware not written).

### T-Dongle-S3 — `models/tdongle-s3/` · shipping · own project
ESP32-S3, 512 KB RAM, no PSRAM, 16 MB flash; 160×80 LCD; **no LoRa**. WiFi +
BLE5. Runs the full `xprs_app`: **BLE5, LAN and ESP-NOW** bearers, digipeater,
cross-bearer bridge, APRS-IS iGate, HTTP API, indexer, **signed OTA**. Designed
to sit on a USB port next to a router as the **BLE↔LAN bridge**; the screen
cycles three status views on its own because the button is usually under a case.
The steadiest choice for the GATT-server role on the bench.

### M5Stack Core — `models/m5stack-core/` · shipping · own project
Original ESP32 (Bluetooth **4.2**, so **no BLE5 bearer**), 320×240 screen, three
buttons. **LAN and ESP-NOW** only, with the seven-panel dashboard, a hotspot AP
with the chat page, digipeater, iGate, indexer and signed OTA. Lives at ~8–15 KB
free heap, so it **takes an OTA push over its own AP rather than fetching** one
(the HTTP client would not fit at the moment the signature is checked). The
"second voice" on the bench: something to hear whatever the S3 boards send.

### T-Deck — `models/tdeck/` · shipping · own project
ESP32-S3 with 8 MB PSRAM, QWERTY keyboard (on its own ESP32-C3), touchscreen,
trackball, and an **SX1262 LoRa** module. The **only board with all four
bearers** (BLE5, LAN, ESP-NOW, LoRa) and the only one you type on, so it carries
the interactive chat panel. Also **serves the BLE GATT mesh channel** the
P1-Pro dials — the reference for the GATT-server side.

### Heltec WiFi LoRa 32 V1 / V2 — `models/heltec-v1`, `-v2/` · legacy · multiboard
Original ESP32 + **SX1276 LoRa** + 128×64 OLED (V2 has 8 MB flash and Vext
control). Bluetooth 4.2, so **no BLE5**. Built by `multiboard` as the older
station (SoftAP web chat, iGate, console); a prebuilt image is in the tree.
Superseded by the V3.

### Heltec WiFi LoRa 32 V3 — `models/heltec-v3/` · shipping hardware · multiboard (pending)
ESP32-S3 + **SX1262 LoRa** + OLED — the **same MCU and LoRa chip as the
T-Deck**, so an `xprs_app` port is "a pin table away". Today it is a `multiboard`
target that does not yet compile; the natural next full station.

### kv4p HT — `models/kv4p/` · legacy · multiboard
ESP32 driving an **SA818 VHF transceiver** — packet on the 2 m band through
`xprs_sa818`, the fleet's only non-ISM radio. Legacy `multiboard` build.

### esp32c3-mini — `models/esp32c3-mini/` · planned · multiboard
The cheapest headless station: RISC-V ESP32-C3, **BLE5 + WiFi, nothing else**.
Firmware not written yet.

### ePaper 1.54" — `models/epaper-1in54/` · legacy · multiboard
ESP32-S3 with a 200×200 e-paper panel, an RTC and sensors. The board the
`multiboard` project's `docs/summary.md` was originally written around
(LVGL UI, WiFi portal, sensor/RTC tasks). Legacy build.

### generic — `models/generic/` · legacy · multiboard
A plain ESP32 devkit with no screen and no radio module — for exercising the
bearers and the shared code on the cheapest possible hardware.

### SenseCAP Solar Node P1-Pro — `models/sensecap-p1-pro/` · shipping · own project
**Not an ESP32.** Nordic **nRF52840** under the Adafruit Arduino core, solar +
battery, weatherproof, headless, on a pole. **LoRa (SX1262) + BLE5**, no WiFi.
Runs the shared `xprs_codec` and `xprs_bearer` unmodified, plus its own key
storage, signing, LoRa+BLE digipeating, and the **GATT over-the-air update** of
§6.2. Its `firmware/README.md` carries the pin map and the three
flash-vs-SoftDevice rules; §7 above is the general version of what it taught.

---

## 9. Where to start reading, by task

| You want to… | Start at |
|---|---|
| Understand the wire format | [`../spec/XPRS.md`](../spec/XPRS.md) |
| Understand a station's duties | [`docs/device.md`](docs/device.md) |
| Add or port a full ESP32 station | `common/xprs_app`, [`docs/esp32.md`](docs/esp32.md) |
| Work on Bluetooth | [`docs/ble5.md`](docs/ble5.md), [`-gatt`](docs/ble5-gatt.md), [`-nrf52`](docs/ble5-nrf52.md) |
| Work on the relay decision | `common/xprs_bearer/xprsbearer.c` |
| Work on signing | `common/xprs_sig`, `common/xprs_id` |
| Push firmware to a WiFi board | `tools/push_firmware.sh`, `common/xprs_ota` |
| Push firmware to the P1-Pro | `tools/push_firmware_p1.py`, `models/sensecap-p1-pro/firmware/src/update.cpp` |
| Bring a new non-ESP32 board up | `models/sensecap-p1-pro/` and §7 above |
