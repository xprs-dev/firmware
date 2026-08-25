# TODO

Work this firmware needs, written down with enough context to be picked up cold.

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
