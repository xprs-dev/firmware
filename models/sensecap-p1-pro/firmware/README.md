# P1-Pro firmware

A headless XPRS LoRa station on an nRF52840. Not an ESP-IDF project — see
`../README.md` for why, and `src/board.h` before touching a pin number.

```sh
~/.platformio/penv/bin/pio run
~/.platformio/penv/bin/pio run -t upload
~/.platformio/penv/bin/pio device monitor
```

| | |
|---|---|
| `platformio.ini` | board, RadioLib, and the two by-id port names this board has |
| `src/board.h` | the pin map, Nordic `Pn.mm` beside the Arduino index that actually addresses it |
| `src/main.cpp` | the station |
| `lib/xprs_codec` | symlink to `common/` — the wire format, unmodified |
| `lib/xprs_bearer` | symlink to `common/` — the relay decision, unmodified |
| `lib/xprs_sig`, `lib/xprs_id` | symlinks to `common/` — the signature and the `sig:` field |
| `lib/xprs_nostr` | symlink to `common/` — only `bech32.c` is built here, for the npub the callsign comes from |
| `lib/mbedtls_ecp` | mbedtls 3.5.2 bignum + ECP over secp256k1, vendored: the curve maths `xprs_sig` needs and the Adafruit core does not ship |

## Console

| key | |
|---|---|
| `?` | status: key, bearers, link, peer, counters |
| `k` | who we are: callsign, npub, boot count |
| `K` | the private half as an nsec — the backup of this station's identity, printed on request only |
| `I<nsec>⏎` | adopt a key: a replacement board keeps the callsign the pole is known by. Writes and reboots |
| `i` | air a `t:identity` now |
| `cfg set/get/list` | the OTA allow-list: `fwkey` (publisher, one 64-hex key) and `own1..own4` (owners, npubs). A `set` writes and reboots |
| `U` | firmware self-test: copy the running image through the real install path and reboot into it on probation (proves the flash path without a push) |
| `b` | bring BLE up (again), watching the SoftDevice's verdict live |
| `d` | dial the last connectable XPRS station heard, over GATT |
| `m` / `M` | send a short / an MTU-sized frame down the link |
| `x` | hang up |
| `D` | reboot into the UF2 bootloader, through the SoftDevice (see below) |

## Bench overrides

The beacon is every five minutes, which is right on a pole and tedious at a
desk. For a validation run:

```sh
~/.platformio/penv/bin/pio run -t upload \
  --project-conf <(cat platformio.ini; echo 'build_flags = -DBEACON_EVERY_SEC=15')
```

or add the `build_flags` line temporarily. Both `BEACON_EVERY_SEC` and
`BEACON_JITTER_SEC` are `#ifndef`-guarded for exactly this.

## The key, and where it lives

Generated once on first boot and kept at `/xprs/key` in the internal
LittleFS, beside `/xprs/boot`, the boots ordinal of XPRS 10.7. Reflashing
the application keeps both; a chip erase does not, which is what `K` is for.
The callsign is `X3` plus the four characters after `npub1` of that key's
npub, the same derivation as `common/xprs_nostr/nostr_keys.c`.

**Every flash write happens before the SoftDevice starts.** With the
SoftDevice up, the core's `flash_nrf5x.c` blocks on a semaphore that only
the SoC event `NRF_EVT_FLASH_OPERATION_SUCCESS` gives, and on this firmware
SoC events are pumped by `tn_gatt_pump()` — on the same task. The first
write after `ble: up` deadlocked the station silently. `keys_init()` runs
first for that reason, and `I` (import) takes the SoftDevice down before it
writes, then reboots.

**The station runs on its own 12 KB task.** The Arduino core gives
`loop()` 4 KB and no way to ask for more; a signature (mbedtls scalar
multiplication plus three wire-sized buffers) overflowed it with nothing on
the port to say so.

## Updating over the air

A pole is hard to reach, so this board takes a new image the way the ESP32
boards do (XPRS 25.8, `common/xprs_ota`) -- the same two-key rule, the same
`xprs_auth` gate -- but delivered as XPRS packets instead of over HTTP,
because this chip has no WiFi. `src/update.{h,cpp}` and `tools/push_firmware_p1.py`.

**Two signatures, two keys, on purpose.** The **publisher** key (`fwkey`)
approves the *image*: a signature over `xprsfw1 <board> <version> <size>
<sha256>`, so a build for one board cannot install on another and last
version's approval cannot be replayed. An **owner** key (`own1..own4`)
authorises *this station* to take it: a signed `cmd:update`, which
`xprs_auth` checks exactly as on the ESP32 -- on the allow-list, signed,
direct (never carried), inside the 300 s window, not a repeat. An `X3`
callsign derives from its key, so the gate is airtight with no
trust-on-first-use. Nothing reaches flash before both verify.

**How the image travels — a 1:1 GATT connection, not the broadcast plane.**
The owner's `cmd:update` names the version, size and sha256; the station
answers `202` with the chunk count. The image then rides a **private BLE
connection** (docs/ble5-gatt.md): the station dials a peer that serves the
`tn_att` mesh channel (FFE0/FFF1/FFF2), and the same `cmd:zfw n:<i>
m:<base85>` frames — 128 bytes each, one per 244-byte GATT frame — cross on
the connection's own 37 channels with acknowledged delivery, not on the
three advertising channels every station shares. `cmd:zfwsig` carries the
approval; `cmd:zfwq` names any missing chunks so only those repeat. The same
frames can also go over the broadcast plane as a fallback, but a connection
is the right place for bulk: it is private, it does not flood the neighbours,
and it does not depend on a gateway's re-air queue.

The receive side is `gatt_rx()` in `main.cpp` handing a `t:command` to
`xfw_gatt_rx()`, and the reply going back over the same link. The image
source on the bench is a T-Deck running `tools/tinynimble_probe` (the proven
`tn_gatt_serve` server) as a transparent serial↔GATT bridge, driven by
`tools/push_firmware_p1.py --gatt <probe-port>`.

**Nothing is trusted until it is whole and approved.** The station installs
only when the staged bytes hash to the `sha:` the owner named *and* the
publisher's approval verifies over that hash. A stranger flooding `zfw`
chunks wastes airtime and nothing else.

**A new image is on probation.** There is no second app slot on this chip,
so the firmware makes one: the region above the application holds what runs,
what is arriving, and a copy of what ran before. A RAM-resident copier
(interrupts and SoftDevice off) swaps the staged image in and keeps the old
one aside. The new image has two minutes with a working radio to prove
itself; if three boots pass without that proof -- a hang, a crash loop --
the copier puts the old image back and the station says so. The watchdog
(60 s) turns a hang into one of those boots.

```sh
tools/push_firmware_p1.py --gateway <ip> --to <callsign> --version <v>     --hex .pio/build/p1pro/firmware.hex     --fw-nsec ~/.xprs/fw.nsec --owner-nsec ~/.xprs/owner.nsec     --from <owner-callsign> --bearer ble
```

Bench-validated 2026-08-30/31: the flash write path byte-exact; the copier
install + reboot + probation + **prove/keep**; and the **fail-to-prove →
restore the previous image** path; the `fwkey`/`own1` allow-list survives a
reflash (a lost key is a ladder, never a brick). Over the 1:1 GATT link: the
signed `cmd:update` accepted, the clock learned from it, `202` answered back
over the link, and the image chunks flowing in order under acknowledged
backpressure. What is not yet closed on the bench is the *whole* 1252-chunk
transfer through to install over GATT -- the shared bench radios were too
unstable to hold a link for the full run. See `docs/ble5-gatt.md`.

Four bugs the chain turned up, all fixed and worth reading:
- **`xprs_auth` freshness underflowed** on a command whose `ts:` was even a
  second ahead of the station's clock (`t - when` on unsigned values). It
  never showed on the NTP-clocked ESP32s; it refused every command on the
  P1, whose clock lags. Now guarded, and the P1 tracks its clock forward
  from each owner command instead of a single early sample.
- **The up-front staging erase starved the radio.** Erasing ~40 pages back
  to back held the SoftDevice off the air long enough to drop the BLE link
  (supervision timeout). Pages are now erased one at a time as their first
  chunk lands.
- **A flash wait reentered the GATT receive callback.** `flash_wait()` used
  to pump the whole BLE queue while waiting for a flash-done event, which
  re-delivered the next chunk into `gatt_rx` from inside the chunk being
  written — corrupting `tn_gatt_pump`'s static event buffer and hanging the
  board under a fast image push. It now drains SoC events only
  (`tn_soc_pump`); a chunk's flash never touches the BLE queue.
- **A blast of chunks overran the bridge.** Sending faster than the link
  drained overflowed the probe's UART and lost most chunks; the probe now
  acknowledges each forwarded frame and the pusher waits for it.

**Three facts about this chip that shaped the install** (`update.cpp` has
them in full): flash while the SoftDevice runs is written through it and
reported by a SoC event, so a write is ask-then-pump; the 0.9.2 UF2
bootloader here ships with its app-CRC check disabled (`bank_0_crc=0`), so
it accepts whatever the copier writes as long as the settings page is left
alone -- the copier must **not** erase it; and every filesystem write
(the key, the config, the probation note) must happen with the SoftDevice
down, or a half-finished write can lose the key. That last rule is why
`keys_init()` and `cfg` run before the SoftDevice and reboot after a write.

## Two things that will waste an afternoon

**The LoRa modem settings must equal `common/xprs_bearer_lora/xprslora.c`.**
SF7, BW 125 kHz, CR 4/5, preamble 8, CRC on. Two radios on one frequency
with different spreading factors are as deaf to each other as two radios on
different bands, and nothing reports it — the symptom is a peer count that
stays at zero, which looks exactly like being out of range.

**If the board hangs on reflash with a dead USB port, press RST.** That was
the Arduino core's `enterSerialDfu()` writing a register the SoftDevice
restricts; the firmware now survives it (`tn_port_sd.c`), but an OLDER image
that does not will need a double-tap on RST into the bootloader and a
direct write: `~/.platformio/penv/bin/adafruit-nrfutil dfu serial -pkg
.pio/build/p1pro/firmware.zip -p /dev/serial/by-id/usb-Seeed_XIAO_nRF52840_*
-b 115200 --singlebank`.

**`upload_port` is the APPLICATION by-id name, not the bootloader's.** The
board answers to `..._XIAO_nRF52840_<serial>` while running and
`..._XIAO-BOOT_<serial>` while in the UF2 bootloader. Pinning the BOOT name
works exactly once — while the board happens to be sitting in it — and then
fails with "No such file or directory" forever after.
