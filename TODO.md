# TODO

Work this firmware needs, written down with enough context to be picked up cold.

## Meshtastic: what is left after the ESP32 half (2026-09-19)

**Status: the ESP32 boards run it and it is bench-validated both ways against
a stock Meshtastic 2.7.26 node and the Meshtastic Android app
(docs/lora.md, "Measured on the bench"). Anything new here follows
docs/lora.md, "The rules we follow", and reads its "Lessons learned".**

0a0. **Taking turns on two networks: what is left.** `[lora] rotate` is
   bench-proven on both sides now (docs/lora.md section 10): the turns,
   the duty ledger across them, the worker staying up, 40% against 80%
   reception, and a stock MeshCore v1.17.1 repeater relaying this
   station's channel messages on its MeshCore turns while hearing nothing
   from it on the other ones, plus a 30-minute unattended soak from the
   config-and-restart path: 72 turns, flat heap, no backwards step in the
   ledger. What that soak showed and nobody should forget: 306 s of a
   360 s hourly allowance spent in half an hour, because one allowance
   covers both networks. A busy site wants `broadcasts_per_hour` lowered
   rather than a bigger radio. The catch-up clients that would make a rotation much less lossy (a
   MeshCore room-server login with `sync_since`, a Meshtastic
   `CLIENT_HISTORY` request on return) are written up in docs/lora.md
   section 10 and not started.

0. **Try the remote LoRa mode switch on the bench.** `cmd:set lora:<mode>`
   is host-tested and in the Firmwares wapp (0.3.6), but neither bench board
   is owned by a profile on the bench phone (X1ARKL) or desktop (X16JK8):
   the T-Deck's owner is `npub1jk77…`, the Heltec's `npub18364…`. Run it from
   the owner's phone: Name screen, LoRa, then watch the 200 come back and
   the station stay up (the switch is live now; there is no restart).
0b. **MeshCore, on the air.** `common/xprs_meshcore` is written and
   host-tested end to end (the wire against the published layouts and
   OpenSSL, the repeater and the bridge against two bridges and a fake
   MeshCore node), and nothing of it has been on a radio. What is NOT
   implemented, and is a decision rather than an omission: reactions
   (MeshCore has no tapback), positions and telemetry, room servers, and
   transport-routed packets.
0b1. **MeshCore mode needs PSRAM, and the Heltec V3 does not have it.**
   The state (7,092 B) plus the worker's 6 KB stack against the ~11.8 KB
   that board has free: the arithmetic does not close, so the mode is
   refused there and the station comes up in its default one
   (docs/esp32.md, "The arithmetic that did not close"). If it should run
   on a board like that, the way in is to make it smaller -- a shared
   worker for both bridges, or MC_SMALL tables trimmed again -- not to
   claim the memory and hope.
0b2. **Auto-detect on the air: done, and it cost three fixes.** 2026-09-20,
   a T-Deck running `cfg detect` against a Heltec V3, first on this
   firmware in `meshtastic` mode and then on the published MeshCore
   v1.17.1 repeater build. Both legs now answer: Meshtastic relayed the
   probe and the sweep left that mode after 6.1 s, MeshCore's stock
   repeater after 2.0 s. Three things the bench found that no host test
   could: the boot frequency was pinned over every mode, so a sweep
   listened to one channel with three modulations; a probe aired about
   30 ms after the retune is transmitted in full and demodulated by
   nobody, so it now waits 1.2 s and repeats once mid-dwell; and the
   Meshtastic probe on XPRS's own channel is swallowed by our own
   stations as half a wire, so it rides LongFast's channel hash instead
   (docs/lora.md, "Auto-detect").

0c. **MeshCore on the air: done, and what it changed.** 2026-09-20, against
   a Heltec V3 running the published MeshCore v1.17.1 (repeater, then the
   companion build driven over USB) with a T-Deck running this firmware:
   contacts, channel messages both ways, direct messages both ways with the
   gateway receipt. A stock repeater does NOT relay our `RAW_CUSTOM`, so
   XPRS on that channel is direct-range only (docs/lora.md,
   "MeshCore, measured on the air"). The bench boards are now: Heltec =
   stock MeshCore (its XPRS identity X3H3MZ is backed up in
   ~/xprs-nvs-backups/2026-09-19/heltec_X3H3MZ_nvs_2026-09-20.bin), T-Deck
   = this firmware as a fresh unowned station X3HW9U, and the Meshtastic
   witness is gone. Put them back before the next Meshtastic session.
1. **Port the P1-Pro to `meshtastic` mode.** It runs the `xprs` LoRa mode
   (SF7), so ESP32 stations set to `lora_mode xprs` hear it and the default
   `meshtastic` ones do not. RadioLib's
   `begin()` on LongFast (SF11, 250 kHz, CR 5, sync 0x2B, preamble 16, at
   `mt_slot_freq_hz()`), `scanChannel()` for listen-before-talk, the
   `xprs_meshtastic` component by symlink into `firmware/lib` (its
   `library.json` is ready), and `xlc_aes_encrypt_block()` over the CC310
   (`Adafruit_nRFCrypto`) or mbedtls's `aes.c`, since `lib/mbedtls_ecp` has no
   AES. It should take `lora_mode` like the ESP32s (both modes, `xprs` kept),
   not trade one for the other. Until then, reach it over the air from a
   station set to `xprs`.
2. **The app on a phone.** Done in the app and the chat wapp, host-tested, and
   checked on the desktop against the bench (app/docs/meshtastic.md section
   5). Not yet seen on a phone, and not yet seen on any screen: the chat's
   Meshtastic room notes and the finder's "via Meshtastic" row were tested in
   the chat harness only. The bundled chat (0.7.28) and archiver (0.7.6) are
   rebuilt in `assets/wapps`.
3. **Soak the Heltec's heap.** With the bridge on, free heap is steady at
   11.8 KB but the minimum ever reached 5.0 KB after 90 s and was still
   falling slowly. Run it for a day and read `min=` on the alive line; if it
   reaches three digits, shrink `MT_SMALL` further or give something else up.
4. **The witness T-Deck** (`DC:DA:0C:39:F6:54`) is on stock Meshtastic. Its
   XPRS identity (callsign X3S7S8) is backed up outside the repository, key
   included, as `~/xprs-nvs-backups/2026-09-19/tdeck2_X3S7S8_nvs_backup.bin`
   (NVS, 0x9000, 24 KB): to restore, flash the T-Deck image by the three-write
   move (docs/esp32.md) and write the NVS back.
5. **kv4p (multiboard) does not build**, and did not before this work:
   `xprs_api.c` calls `esp_core_dump_get_summary()` and the kv4p sdkconfig has
   `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` unset (docs/esp32.md says it is not
   optional on any board). The SA818 gate now also refuses `MT`/`MC` senders.
6. ~~`common/xprs_bearer_lan/test_xprslan_host.sh` does not compile~~ Fixed
   2026-09-20: `s_lan` lived inside the device-only branch, so the host
   build had no definition for the wrappers at the bottom of the file. It
   is now defined above the split, and the harness runs (76 checks). Every
   host test in `common/` is green.

## Measure the long-range PHY

**Status: the code is written and has never been proven to do anything.**

This is the question the whole ESP-NOW effort was for, and it is still
unanswered: does the long-range PHY buy enough range to be worth a quarter of a
megabit?

### What exists

Section 23.7 of the specification moves a pair of stations to a working channel
of their own, and `common/xprs_chan/` implements it. The invitation can
ask for the long-range PHY -- `chan <peer> <channel> [seconds] lr` on the
T-Dongle console (`models/tdongle-s3/firmware/src/main.c`, the `chan` command)
sets `lr`, which reaches `xc_set_lr()` in `common/xprs_chan/xprschan.c`:

```c
uint8_t bitmap = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
if (on) bitmap |= WIFI_PROTOCOL_LR;
esp_wifi_set_protocol(WIFI_IF_STA, bitmap);

esp_now_rate_config_t rc = {
    .phymode = on ? WIFI_PHY_MODE_LR : WIFI_PHY_MODE_11B,
    .rate    = on ? WIFI_PHY_RATE_LORA_250K : WIFI_PHY_RATE_1M_L,
};
esp_now_set_peer_rate_config(k_broadcast, &rc);
```

Both calls return `ESP_OK`. **That is the entire basis for believing LR works.**
Nothing has ever read the PHY back, and nothing has ever compared a packet sent
under it to one sent without. "LR works" is an assumption wearing a log line.

### What has to be built

Two pieces, neither large.

**1. Read the configuration back.** `esp_wifi_get_protocol()` after setting it,
logged as a bitmap. Without this the comparison might be between two identical
configurations, and would look like "LR makes no difference" rather than "LR was
never applied". Do this first; it is five lines and it can invalidate the rest.

**2. A measured burst on the working channel.** Today the pair meets and then
sits there -- the move buys nothing and there is no instrument. The burst is
both the work and the instrument:

- on `on_working`, the inviter airs N numbered packets as fast as the bearer
  accepts them: `t:message f:X3WWAJ d:X3LTSH m:burst 37/100`. No re-air jitter
  -- the channel is private, which is the point of having moved
- the invitee counts distinct sequence numbers and accumulates RSSI in RAM
- back on the calling channel it airs one summary:
  `t:report ... m:heard 94 of 100, -71 dBm avg`

Both `t:message` and `t:report` are already assigned; **no new vocabulary**,
which the house rule requires (show an example packet and get agreement before
adding any XPRS key or type).

### The measurement

Two boards, one variable, at each of two distances:

| | delivered | RSSI | elapsed |
|---|---|---|---|
| normal rate | | | |
| `WIFI_PHY_RATE_LORA_250K` | | | |

The second distance matters more than the first: pick one where the normal rate
starts losing packets. Equal delivery at close range proves nothing -- both PHYs
work fine on a bench.

### Traps already paid for

- **Stopping Bluetooth is not optional.** With the BLE controller running, a
  WiFi station that is not associated receives NOTHING while transmitting
  perfectly. `xprs_chan` takes it down for the exchange and brings it
  back; see `docs/espnow.md` for the truth table from `tools/espnow_probe`.
- **The return path is fragile.** An illegal phymode/rate pairing on the way
  home (`WIFI_PHY_MODE_11G` with `WIFI_PHY_RATE_1M_L`) once left the broadcast
  peer misconfigured and ESP-NOW deaf until reboot. That is why the way back is
  `WIFI_PHY_MODE_11B`. If LR is changed, re-check the return.
- **`esp_now_send()` is asynchronous.** Use `xprsnow_settle()`, not a delay.
- Measurement discipline in `docs/esp32.md` is binding: no serial port open
  while measuring, reachability reported as *n* of *m*, heap read before
  believing anything.

### Hardware

The pair used throughout: T-Dongle-S3 (`X3WWAJ`, `/dev/ttyACM0`) and M5Stack
Core (`X3LTSH`, `/dev/ttyUSB0`). Open each port ONCE and leave it open -- a
shell redirect to the M5Stack's CP2104 asserts DTR and reboots the board, which
silently invalidated a whole run before it was noticed.

The rendezvous itself is reliable enough to build on: ten consecutive attempts
gave eight meetings, and `nobody came` -- the failure that dominated this work --
has not occurred since the Bluetooth fix.

### Done when

The table above is filled in, at two distances, with the PHY bitmap logged for
both rows, and `docs/espnow.md` carries the numbers.

## Spec proposal: signal per callsign (from xst_hears_render)

The firmware airs `zhq:` beside `hears:` under the private z prefix -- one
digit a callsign, same order, same count, 9 loud to 0 barely there, about
7 dB a step from -30 dBm to -100. Worth proposing for XPRS.md 10.6.3 once
agreed, as `hq:`:

- **`hq:` on `t:observation`** -- 10.6.3 declined this once: "Signal per
  callsign is deliberately not carried: it would need a compound value
  this format does not have." A positional digit string is not a compound
  value. It is one token, one type (digits), read against `hears:` by
  position, and 4.3 already carries two-part values in `coord`, `ratio`
  and `epoch` without anybody calling those compound.
- **Ordering guidance for 10.6.3** -- the section leaves "most relevant
  first" to the sender and lists signal, uptime and whether the station is
  powered among the criteria. Worth naming the one that is not a
  judgement: section 2 already says `X3` is a station, relay or unattended
  equipment and `X1` is a person, so a cut list that keeps the `X3`s keeps
  the carriers. This firmware ranks `X3`, `X4`, `X1`, then loudest, then
  freshest.
- **`q:hears`** for section 8 -- 10.6.3 says the full list reaches an
  archiver "over section 6.6 parts", but section 8 assigns no word to ask
  for it. Not implemented here: there is no 6.6 part support in this
  firmware, and with a 16-row store against 19 callsigns a packet the
  truncation it would relieve cannot happen on an ESP32.

What the ladder does when the packet is full -- digits go before names,
names go last, `peers:` stays true throughout -- is 10.6.4 as written and
needs nothing new. Example packets, byte counts and the reason the buckets
are coarse (raw dBm defeats an archive's repeat detection) are in
docs/espnow.md, "Its own beacon".

## Spec proposal: diagnostics commands (from common/xprs_diag)

The firmware answers `cmd:zdiag`, `cmd:zcore`, `cmd:zlog` under the
private z prefix. Worth proposing for XPRS.md 25.2 once agreed:

- `cmd:diag` -- one frame of station state; `fw: uptime: peers:` are
  assigned keys already, the health word and heap figures need names.
- `cmd:log since: until:` -- lines in `m:`, paged exactly as 25.2.1.
- `cmd:trace` -- the crash summary: task, PC, backtrace.
- `uptime:` on `t:service` (it is a 10.5 observation key; 25.8 put `fw:`
  there with the same argument).

Example packets and byte counts are in docs/device.md, "Diagnosing over
the air".

## Found on the bench, 2026-08-30 (the LoRa -> BLE -> LAN chain test)

Still open:

- **`wifi_on=0` does not keep the deck off the LAN.** `wifi_up()` starts the
  driver for ESP-NOW with a blank SSID, and the driver auto-associates from
  its OWN NVS copy of the last credentials ("config NVS flash: enabled" ...
  "connected with ---___---"). Fix: `esp_wifi_set_storage(WIFI_STORAGE_RAM)`
  before start, or an explicit disconnect. Until then "WiFi off" is a lie
  the health line repeats.
- **A typed chat message rides `idx_task`**, and `idx_task` can wedge on
  `rns_tcp` DNS while WiFi is flapping ("chat: the last message has not
  gone out yet", then TASK WDT in `idx`, crash in `rns_tcp`). The outbox
  should not depend on the task that also talks to the internet.
- **The desktop app leaks HTTPS connections**: 1,323 TCP sockets in
  CLOSE_WAIT to four hosts on :443 after five hours. xprs-flutter.

Fixed the same day:

- A station relayed its own origin (`f:` was not counted as "in the path";
  xprs_codec's `xprs_append_via` now refuses the author's own packet).
- `/api/xprs/send` blocked the httpd task on an index write; our own sends
  now go through the index queue like everything else, and the door answers
  in 0.2 s.
- The desktop app's LAN socket died silently and stayed "up"; it reopens
  now (xprs-flutter).

## Flash the P1-Pro once by cable, then prove the no-cable update

**Status: step 1 is done (2026-09-05, by cable, 0.2.0 running); step 2 is
the open one.**

The remote-update feature (`models/sensecap-p1-pro/firmware/src/update.{h,cpp}`,
`tools/push_firmware_p1.py`, commit bc960b6) was tested over the air on
2026-08-31 and every leg up to the station's door works: both signatures
(publisher approval + owner cmd:update, test keys in `~/.xprs`), the gateway
send (`"bearers":"ble"` from the T-Deck), and the station hearing it
("hears:...,X38364" in its own beacons). It never answers, because the image
running on the battery predates the broadcast intake (`xfw_handle` in
main.cpp) -- and the GATT fallback needs a console keypress ('d' to dial),
which a pole unit does not have.

### The steps

1. ~~Plug the P1-Pro into USB-C and flash 0.2.0.~~ **Done 2026-09-05.**
   `~/.pio-venv/bin/pio run -t upload --upload-port /dev/serial/by-id/usb-Seeed_Studio_8044_61022839E75C91AE-if00`
   from `models/sensecap-p1-pro/firmware` (the 0.1.6 image enumerated under
   that `8044` name, not the pinned `XIAO_nRF52840` one; 0.2.0 enumerates
   under the pinned name again). Key and callsign survived (`X3S7S8`, boot
   36), no probation on a cable flash. Both directions to the T-Deck were
   measured before and after -- see the P1-Pro README, "Measured … 2026-09-05".
   The earlier "deaf on 868.0" note was wrong for this unit: 0.1.6 already
   sat on 869.5 and exchanged LoRa with the T-Deck.
2. Bump `version.txt` to 0.2.1, `pio run`, then the real test with no cable
   anywhere:
   `tools/push_firmware_p1.py --gateway <tdeck-ip> --to <callsign>
    --version 0.2.1 --hex .pio/build/p1pro/firmware.hex
    --fw-nsec ~/.xprs/fw.nsec --owner-nsec ~/.xprs/owner.nsec
    --from X38364 --bearer ble`
   Expect ~1,300 chunks at 4/s (~6 min), zfwq resend rounds, then the
   probation boot and prove/keep. Watch for the answer wires in the
   gateway's history; the P1's beacons name the running version.
3. If the broadcast plane disappoints, the GATT road is one flash away:
   `tools/tinynimble_probe` env `deckB` onto the T-Deck, 'g' to serve,
   the station dials, then `--gatt /dev/ttyACM0` instead of a gateway --
   and remember to reflash the T-Deck back to the station afterwards.

Auto-dial (the station dialling a heard probe without a console) is the
follow-up worth considering while in there: it would make the GATT road
remote too.
