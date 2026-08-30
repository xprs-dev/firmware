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
