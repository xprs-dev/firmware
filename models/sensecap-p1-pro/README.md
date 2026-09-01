# SenseCAP Solar Node P1-Pro

A 5 W panel, four 18650 cells and a LoRa node in a weatherproof case on a
pole bracket. The fleet's first **headless** station: no screen, nothing to press, left
outdoors and expected to still be there in a year. Also its first non-ESP32,
and the first that runs the shared XPRS code on a chip the shared code was
not written for.

It works. A station on this board and a T-Deck on the same bench hear each
other on LoRa in both directions, and a beacon composed here has been
digipeated by an ESP32 onto the LAN.

| | |
|---|---|
| Chip | **nRF52840** -- not an ESP32 |
| Radios | LoRa SX1262 (862-930 MHz), Bluetooth 5. **No WiFi.** |
| `board.yml` | the catalogue entry, machine-readable (`docs/catalog.md`) |
| `hardware/` | the full pin map, the block diagram, photographs |
| `firmware/` | PlatformIO, Arduino/Adafruit-nRF52, env `p1pro` |

## Two things about this board that are not like the others

### It is not an ESP32

Every other board in `models/` is an Espressif part and every component in
`common/` is an ESP-IDF component. None of that compiles for a Cortex-M4
with Nordic's SDK, so this board's firmware is not the T-Dongle's `main.c`
with the pins changed -- it is a PlatformIO project under the Adafruit nRF52
Arduino core, with the shared code arriving through `lib/` instead of
`components/`.

**Two files crossed over unmodified**: `common/xprs_codec` and
`common/xprs_bearer`, symlinked from `firmware/lib/`. That is the wire
format and the relay decision -- the duplicate rings, the section 13.2.1
random wait, the cancel when somebody else gets there first, and appending
this station to `via:` -- running on a chip they were not written for, and
producing packets an ESP32 accepts as its own.

Getting them there cost exactly two changes, both in `common/` and both
small enough to read in a minute:

- `xprs_codec` gained `xprs_sha256_sw.c`. The codec always took its one hash
  through `xprs_sha256()`, a seam rather than a dependency; on the ESP32s
  that seam is filled by mbedtls, and the host harness had its own copy
  inside the test file. That copy is now a file the codec can offer to any
  target without mbedtls. Checked against the FIPS 180-4 vectors, and the
  IDF build still takes `xprs_sha256_idf.c` and is untouched.
- `xprs_bearer`'s two log lines learned that a target which is neither the
  IDF nor the host harness should fall silent rather than fail to find
  `esp_log.h`.

Nothing else moved, and all three ESP32 boards build unchanged.

`xprs_sig` is the next one over, and the shape is already there: it picks
OpenSSL on the host and mbedtls on the ESP32s by preprocessor, so a third
backend is a third branch rather than a rewrite.

### It has no WiFi, and that is structural

`xprs_bearer_lan` and `xprs_bearer_now` cannot exist here, which is obvious.
What is not obvious is what that does to the SHARED STATION: in
`xprs_app.c`, the LAN bearer's task is what pumps every other bearer's
re-air queue and beacon timer. `xapp_run()` says so out loud --

> The LAN bearer first, deliberately: its task is what pumps every bearer's
> re-air queue and beacon, ESP-NOW included.

so anyone who ports `xprs_app` to this board gets two bearers with nothing
driving either, and the symptom is silence rather than an error. This
firmware sidesteps it rather than fixing it -- it is not `xprs_app`, and it
calls `xb_tick()` from its own `loop()` -- but the day the shared station
wants to run here, that pump has to come out of `xprs_bearer_lan` and get a
task of its own. It would tidy the ESP32 boards too.

## What it does today

A headless LoRa + BLE5 station, bridging between the two and digipeating
on each. On LoRa it listens on 868 MHz, keeps what it hears out of its own
duplicate rings, repeats within the hop budget with itself appended to
`via:` (section 13), and every five minutes beacons a signed
`t:observation` naming who it hears directly (`hears:`, 10.6.3). It has a
key: an `X3` callsign derived from it (section 3), a signed `t:identity`
30 s after boot and every 30 minutes (9.3), and `epoch:` dating from a boot
counter (10.7) since it has no clock.

**Measured on this bench, 2026-08-30, against a T-Deck:**

| | |
|---|---|
| P1-Pro → T-Deck | 6 of 7 beacons, −30 dBm, SNR 12, byte-exact |
| T-Deck → P1-Pro | 29 wires in 180 s at −31..−33 dBm, none corrupt, none unparsed |
| and onward | `xprs: lan 192.168.178.102 58B t:observation f:X54W6W link:lora peers:0 via:X3GSLC,X3WWAJ` |
| **LoRa digipeat** | T-Deck `t:message f:X3GSLC` on LoRa alone → repeated by this station on LoRa **and** BLE as `via:X33ESX`; T-Deck heard both back (`xprslora: RX 70 bytes at -45 dBm ... via:X33ESX`, `ble -73 dBm ... via:X33ESX`) |
| **Signature** | `t:identity f:X33ESX epoch:1.67 k:npub13esx… sig:…` aired here verifies on the host through the OpenSSL branch of the same `xprssig.c`; a one-character tamper fails. T-Deck's own beacon then lists `hears:X33ESX,X1VCVM` |

(Earlier rows say `X54W6W`: that was the FICR-derived `X5` callsign this
board wore before it had a key. `X5` is a group prefix, not a station's.)

That last line is the one worth having. A packet this chip composed went out
on LoRa, was picked up by an ESP32, digipeated with two callsigns appended
to `via:`, and put on the LAN. The shared codec and the shared bearer
produced a wire the rest of the fleet treats as one of its own.

It costs 83 KB of flash and 11 KB of RAM -- 10% and 5% of what the board has.

### One bug, found on the hardware and not before it

DIO1 is wired to TxDone as well as RxDone, so the blocking `transmit()`
raised the same line the receive path watches. The station then believed a
packet had arrived, asked the radio how long it was, and read a length
belonging to one frame out of a FIFO holding another. It showed up exactly
once, and looked like nothing:

```
lora rx -33 dBm 141B t:observation f:X54W6W link:lora peers:0hears:X3WWAJ ...
```

-- this station's own 40-byte beacon with the tail of somebody's 141-byte
observation welded on at the missing space. A corrupt wire is not a crash;
it parses or it does not, and the next log line looks fine either way. The
fix is one line and it is commented where it sits.

## What is not here yet

- ~~Remote firmware update~~ **Done**: the ESP32's own scheme (two keys, the
  `xprs_auth` gate, XPRS 25.8), delivered as XPRS packets over LoRa or BLE
  instead of HTTP, with a staged image, a RAM copier, and probation +
  rollback on a chip with no second app slot. `firmware/README.md`,
  `firmware/src/update.cpp`, `tools/push_firmware_p1.py`. Bench-validated
  end to end 2026-08-31: a new image pushed to the pole node over a private
  1:1 BLE GATT connection (not the broadcast plane), installed, booted, and
  kept -- callsign and keys intact across the update.

- ~~Signing~~ **Done**: `lib/mbedtls_ecp` is a cut-down mbedtls, and
  `common/xprs_sig` runs on it unchanged bar an `ESP_PLATFORM` seam for the
  hash, the entropy and the log. Key on the internal LittleFS, callsign
  from it, beacons and identity signed. `firmware/README.md` records the
  two things it cost (flash before the SoftDevice; a task of its own).
- ~~BLE5~~ **Done**: `common/tinynimble/tn_port_sd.c` drives the
  SoftDevice directly (Bluefruit is capped at 31 bytes); the beacon goes out
  as an extended advert, is digipeated by the T-Dongle, and BLE<->LoRa
  bridging works. The mesh channel over a connection is measured against a
  T-Deck too: MTU 247, 244-byte frames both ways. `docs/ble5-nrf52.md`,
  `docs/ble5-gatt.md`.
- **Signing.** Packets go out unsigned, which the spec allows and every
  receiver can see. Needs a secp256k1 for this chip; `xprs_sig`'s third
  backend is a third branch, not a rewrite.
- **GNSS.** Wired, powered through a TPS22916 load switch, and held OFF. It
  is a receiver, not a bearer, and on 5 W of sun an unused receiver that is
  merely asleep is a current draw nobody chose.
- **Battery reporting.** The divider is unverified on this carrier (see
  `hardware/HARDWARE.md`). Reporting a percentage from an unverified divider
  on a solar node is worse than reporting none.
- **Idle and TX current.** No published figure, and none measured. For a
  solar node this is the number that decides whether the design works.

Two board facts shape all of the above more than the radios do. There is no
screen and no reachable button -- the user button is inside a sealed case --
so anything the station wants to say has to go over the air or through two
LEDs; and there are only two, because the charging, charge-done and
solar-present LEDs are wired to the CN3165 charger and firmware cannot see
or drive them.

## Building and flashing

```sh
cd firmware
~/.platformio/penv/bin/pio run
~/.platformio/penv/bin/pio run -t upload
```

No `esptool`. The XIAO carries an Adafruit UF2 bootloader; the upload opens
the port at 1200 baud to drop the board into it and then writes with
`adafruit-nrfutil`. Copying the `.uf2` onto the mass-storage volume by hand
after a double tap on reset does the same thing, and is the fallback when
the port will not enumerate.

**The board has two by-id names and they are not interchangeable** -- running
the application it is `..._XIAO_nRF52840_<serial>`, in the bootloader it is
`..._XIAO-BOOT_<serial>`, a different VID:PID. `platformio.ini` pins the
application one deliberately; see the note there.

It shipped pre-flashed with Meshtastic, so there is a known-good image to go
back to.

## Sources

Everything in `hardware/HARDWARE.md` and `board.yml` comes from the vendor,
not from the bench:

- [Product page](https://www.seeedstudio.com/SenseCAP-Solar-Node-P1-Pro-for-Meshtastic-LoRa-p-6412.html)
- [Industrial datasheet, SKU 114993633](https://files.seeedstudio.com/Bazaar/product_pdf/114993633.pdf) -- the block diagram, and the source of the pin map
- [Meshtastic device page](https://meshtastic.org/docs/hardware/devices/seeed-studio/sensecap/solar-node/)

`docs/esp32.md` is binding on how measurements are taken in this tree. The
LoRa figures above were taken that way. Everything still blank in
`board.yml` -- the weight, the IP rating, the current draw -- is blank
because nobody has measured it, not because it was forgotten.
