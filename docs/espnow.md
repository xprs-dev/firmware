# XPRS over ESP-NOW

Every ESP32 has this radio already. ESP-NOW is Espressif's connectionless mode:
802.11 action frames with no association, no access point and no DHCP -- a device
puts a frame on a channel and every ESP32 listening on that channel hears it.

[ble5.md](ble5.md) is the Bluetooth bearer's page and [lan.md](lan.md) is the
LAN's; this is ESP-NOW's. [XPRS.md](XPRS.md) already assigns it: `link:espnow`
is a bearer word (section 10.6.1), and section 23.7 is what to do when a pair
wants a channel of their own.

**The number that makes this a good fit:** an ESP-NOW frame carries **250
bytes**, and the longest XPRS packet is **250 bytes** (section 4). One packet is
one frame, verbatim, with nothing to fragment and nothing to reassemble. The
Bluetooth bearer cannot say that -- its advert header costs six bytes, so a
full-length packet does not fit at all.

## What this is not

**Not a hotspot, and that is the point.** A SoftAP has a client ceiling and
divides one channel's bandwidth among everyone associated to it. Nobody
associates here. The peer table needs exactly **one** entry -- the broadcast
address -- so the 20-peer limit never binds however many stations are listening,
and there is no ceiling to run into.

**Not promiscuous mode.** Broadcast frames arrive through the ordinary receive
callback; sniffing 802.11 would add nothing except CPU on the processor the
radios need. The one thing promiscuous mode would have been for arrives free:
the receive info carries **RSSI**, so this bearer reports signal where the LAN
reports nothing.

**Not Reticulum, and not the internet.** A packet here reaches the ESP32s in
range on one channel and stops.

## The wire

```
ESP-NOW broadcast to ff:ff:ff:ff:ff:ff
one XPRS packet per frame, verbatim, no header
```

Unencrypted, because broadcast always is. Authorship is `sig:` (section 9.1),
which is the answer to "who sent this" on every bearer and does not become the
transport's job here.

## Its own channel, on the long-range PHY

*Since 2026-11-10. Older stations (1 Mbps, on the router's channel) and these
do not meet over ESP-NOW; that was accepted.*

**Long range.** Every frame goes out on Espressif's long-range PHY at
250 kbps (`esp_now_set_peer_rate_config`, `WIFI_PHY_MODE_LR`), and the station
interface lists LR among its protocols so the radio receives it. Four times
the airtime of 1 Mbps (about 9 ms for a 250-byte packet) for up to 6 dB more
link budget; Espressif quotes a few dB in practice. A radio with LR among its
protocols still hears 1 Mbps frames and still joins an ordinary router. A radio
without it hears no LR frame at all.

**The home channel.** ESP-NOW lives on channel 13 (`espnow_ch`), the least
used 2.4 GHz channel in Europe: routers choose 1, 6 and 11. The chip's default
country is "world safe" (channels 1 to 11), so the station sets `wifi_country`
(default `DE`); where the country has no 13 the home channel is 11. A
standalone hotspot opens on the home channel too.

**A station on a router** cannot stay on 13 unless the router is on 13: one
radio, one channel, and in 802.11 the access point chooses it. Asking for 13
when joining (`sta.channel`) was tried: the FRITZ!Box on 11 kept 11. A router
set to channel 13 by hand puts everything on one channel and no visit is
needed. Otherwise the station keeps the router's channel for WiFi and **visits the
home channel for 80 ms (`espnow_win`) at the start of every second** of the
wall clock (SNTP). Every station that knows the time queues its ESP-NOW frames
and sends them inside that window, 25 to 35 ms in, so the visitors are there
to hear them; the radio's own carrier sense spreads them further. The
driver refuses `esp_wifi_set_channel()` while associated, so the visit is a
remain-on-channel request (`esp_wifi_remain_on_channel`, the driver's private
API in IDF 5.2, as DPP uses), which returns to the router's channel by itself. 8 % of the
time on the home channel, and in practice 15 to 29 % off the router's (below).

**A station without a clock** (no router, no internet) learns the window from
what it hears: a frame arrives about 25 ms into its sender's window. Until it
has heard anything it sends at once, which reaches every station sitting on the
home channel.

| setting | default | |
|---|---|---|
| `espnow_ch` | 13 | the home channel |
| `espnow_lr` | yes | send and receive the long-range PHY |
| `espnow_win` | 80 | ms each second a station on a router spends on the home channel |
| `wifi_country` | DE | the WiFi country (channel 13 needs one that has it) |

**WiFi in use: the visit waits.** A visit costs more than its 80 ms. The
driver tells the access point the station is away and back (the access point
holds what arrives meanwhile and hands it over after), and measured on a C3
behind a FRITZ!Box nothing addressed to the station arrived for 150 to 290 ms
of every second. A page load or a Reticulum transfer waits those out one
after another. ESP-NOW is there for range and visitors over WiFi are few, so
while one is being served the station stays with it (decided 2026-11-11):

- **A hold** (`xprsnow_hold(ms)`): whoever knows the station is in use says
  so, and no visit leaves the router's channel until the hold runs out. A
  Reticulum link open to the station renews it with every frame off the hub
  (5 s), a request run for it too, and so does every connection the web
  server takes (the page, its API, a file). Traffic at 12 unicast frames a
  second or more within a minute of a hold is the same visitor still at it
  and holds too. Visits start again about five seconds after the last use.
- **Traffic nobody announced** (a ping, some other service): the station
  counts the frames addressed to it alone, where lwIP is handed them; the LAN
  bearer's broadcast chatter does not count. A second that brought 12 or more
  puts the next visit off, 5 in a row at most, then one visit whatever the
  traffic.

What the visits are not is the cause of the pings a C3 loses under load
(63 % and 77 % when first measured, and blamed on them). With the home
channel set to the router's, so that no visit happens at all, the same pings
lost 0 to 2 % while the board had 17 to 20 KB of heap free and 51 to 87 %
while it had 8 KB; with visits held during an open link they were lost just
the same. That loss follows the heap, and it is open.

What ESP-NOW would have sent meanwhile waits in the window queue (8 frames;
past that a frame is dropped and counted, as a busy radio would) and goes
out at the first visit after. A station nobody is talking to visits
every second as before, and nothing changes on the air: the home channel,
LR and the window are where they were.

The station's 15-second line carries the window's counters:
`espnow home=13 lr window visits=.. switch_fail=.. sent=.. dropped=..
put_off=.. wifi_rx=../s peak=..` (visits put off, and the unicast frames in
the last second and in the busiest one since the line before).

Moving a pair to a working channel of their own is section 14.7's `t:channel`
invitation; with a home channel already off the routers' ones it is wanted
less, and it is not built.

## Not everybody at once

Identical to the LAN, and for the same reason -- every station on the channel
hears the same frame at the same instant:

| | |
|---|---|
| A packet from another bearer | waits **200-1200 ms**, chosen at random |
| The same packet heard meanwhile | the waiting copy is **dropped** |
| A packet this station composed | goes out **immediately**, with no `via:` |

That logic is not written twice: `xprs_bearer` holds it once and both
bearers use it. The identifier compared is the section 5 one, which ignores
`via:` and `sig:`, so a relayed copy is recognisably the same packet.

The delay is uniform, so the station that wins is the random one rather than the
one whose re-air would reach furthest. the retired XPRS-WiFi notes once set that against
Wi-Fi 8's multi-AP coordination and works out what a rank would cost.

## `scope:local` crosses to it

Section 13.11.1 lists Bluetooth LE, WiFi Direct, WiFi Aware and a local network,
and does not name ESP-NOW -- the word did not exist when that list was written.
On the channel the station is already on, ESP-NOW is plainly one of these: no
gateway, no carrying, out of range and gone. So **a `scope:local` packet may be
put on ESP-NOW.**

That ruling is about *this* use of it. Section 23.7's working channel with the
long-range PHY can reach a kilometre, which is not what `local` was protecting,
and deserves its own answer when it is built.

## Its own beacon

The station says who it is and who it hears, per bearer:

```
t:observation f:X3WWAJ link:espnow peers:1 hears:X1RD89 sig:<60 characters>
```

`link:espnow` rather than a shared reading, because section 10.6.1 is explicit
that a reading belongs to the bearer it names -- who this station hears over
ESP-NOW is not who it hears on the wire, and one figure covering both would be a
quantity nobody can act on.

### How well, not only who

`hears:` says who is in reach and nothing about how well, so a reader choosing a
carrier cannot tell the neighbour across the room from the one that barely
decoded. `zhq:` says it -- one digit per callsign, same order, same count, nine
loud and zero barely there:

```
t:observation f:X3WWAJ link:espnow peers:5 hears:X3LTSH,X3R8XX,X1GUD9,X1RD89,X5A3F2 zhq:97542 sig:<60 characters>
```

158 bytes. About **7 dB a step** from -30 dBm down to -100. It is private
vocabulary under the `z` prefix (section 4.9) until the packets have been shown
and agreed; the proposal is in [`TODO.md`](../TODO.md). Section 10.6.3 refused
per-callsign signal once -- "it would need a compound value this format does not
have" -- and a positional digit string is the answer to that objection rather
than an argument with it.

**Coarse on purpose, and that is the interesting part.** The archive drops a
repeated observation by hashing the wire, skipping only `ts:` and `epoch:`
(`xi_presence_hash`). Raw dBm moves every minute, so a beacon carrying it would
be a NEW record every minute in every archive that heard it -- which is the exact
flood that function was written to stop. A bucket moves only when the signal
changes tier, and it is sticky by half a step so a neighbour parked on a boundary
does not retype the beacon either.

`zhq:` is omitted entirely when any listed station has no signal to report, so a
`link:lan` observation never carries it. A hole in a digit-per-callsign string
would be worse than no string.

### When it does not all fit

250 bytes is 250 bytes, and a busy street has more neighbours than that. What
gives way, in order:

| | |
|---|---|
| everything fits | the full list, each callsign with its digit |
| the digits do not fit | **the digits go first** -- the whole list, no `zhq:` |
| the list does not fit | the top-ranked that fit; `peers:` still says how many there were |

Signal is dropped before names because a name is what a reader can act on --
`hears:` is how a message finds a carrier (36.12), and a station left out of the
list cannot be routed through at all.

**Most useful first** is left to the sender by 10.6.3, and this station reads it
as: `X3` before `X4` before `X1`, then loudest, then freshest. Section 2 says
`X3` is a station, relay or unattended equipment and `X1` is a person, so when a
list is cut it is the relays that survive it -- a phone in somebody's pocket is
a worse carrier than the board on the roof however loud it is right now.

The signed observations also **slow down as the room fills**: sixty seconds a
turn, one turn sat out per four neighbours, never slower than eight minutes. The
payload was never the airtime problem -- each turn airs one observation per
bearer over every lane, so nine signed packets can leave before any of them
grows a byte. A station alone on a hill keeps the full minute, which is where
the news is; a station among a dozen others that all hear each other is the one
saying the least surprising thing.

On the numbers above -- a signed observation from a six-character callsign has
141 bytes for the whole suffix -- **19 neighbours fit bare and 16 fit with their
digits**. The store holds 16 (`XST_SEEN_MAX`), so on this hardware the first rung
always wins and the other two are for the stations that are not an ESP32.

## What crosses to the other bearers

Both ways, under the ordinary relay rules: a packet heard on ESP-NOW is offered
to Bluetooth and the LAN, and a packet heard on either is offered to ESP-NOW.
The bearer refuses when this station is already in `via:` or the type's hop
budget is spent (section 13.1, 13.2), so the offer is unconditional and the
decision is one place.

Measured on a T-Dongle and an M5Stack in the same room: a phone's Bluetooth
beacon reached the M5Stack over ESP-NOW with `via:X3WWAJ` on it, and the
M5Stack's own ESP-NOW beacon came back to it over the LAN. Three bearers, one
packet, one relay.

## On the hardware

| | |
|---|---|
| Component | `xprs_bearer_now`, on `xprs_bearer` |
| T-Dongle-S3 | ESP-NOW + BLE5 + LAN, all three |
| M5Stack Core | ESP-NOW + LAN. An original ESP32 has **no** BLE5 extended advertising, so it can never join the Bluetooth plane |
| Cost on the dongle | ~9.7 KB of heap: free 26.8 KB -> 17.1 KB, low-water 20.0 KB -> 10.3 KB |
| Reachability | 120 of 120 with the bearer running -- the same as idle |

**Modem sleep is turned off** (`esp_wifi_set_ps(WIFI_PS_NONE)`). A station that
sleeps misses frames that arrive while it is asleep, which Espressif's own
example warns about. On a board that also runs Bluetooth this is a coexistence
decision, not only a power one -- it was measured rather than assumed, and it
cost nothing: average ping time improved, which is what an always-awake station
would do.

**The receive callback runs in the WiFi task, on core 0.** It copies the frame
into a queue and returns; the identifier (a SHA-256), the archive and the relay
decision all happen on the bearer task. Doing that work in the callback is the
mistake `esp32.md` measured at 1 of 96 pings, in the worst available place.

**No bearer task of its own.** The LAN bearer's task pumps every registered
bearer, because a second task is 5 KB of a heap that has been down to hundreds
of bytes. If no task has claimed that job, `xprsnow_start()` says so as an error
rather than letting a silent failure be discovered in the field.

## `sent=` is not `tx=`

`esp_now_send()` returns once the frame is queued, not once the radio has
finished with it, so `tx` only ever counted what this station *decided* to say.
`esp_now_register_send_cb` counts what actually left, and both heartbeats now
print `sent=<done>/<issued> fail=<n>`. `xprsnow_settle()` waits on the same
counters, which is what section 23.7 uses instead of the 120 ms delay it used to guess
with -- retuning the radio out from under an untransmitted acceptance is a real
failure and it looked exactly like the far side not answering.

The failure that made this necessary: every `esp_now_send()` on the dongle
returned `ESP_ERR_ESPNOW_NO_MEM` for four minutes while `tx=0`, and the log said
nothing, because that branch was `ESP_LOGD`. **A refused transmission is now
`ESP_LOGW`, including a full driver queue.** See `esp32.md` -- the cause was
heap, and one of the things eating it was this bearer's own receive queue.

## Watching a rendezvous: `xprsnow_set_trace()`

Between "the radio never heard it" and "it was heard and refused" no counter can
tell you which, because a frame can be lost before anything counts it: the
callback's `xprs_looks_like` byte test, a full queue, a parse that fails. With
the trace on, every drained frame prints its length, RSSI and first 56 bytes,
and the frames that never reached the queue print as a change:

    heard 107 bytes at -55 dBm: t:receipt f:X3LTSH d:X3WWAJ r:012c52 s:ack
    radio delivered 8 frame(s) (+1), 0 not XPRS (+0), 0 dropped by a full queue (+0)

`xprs_chan` switches it on for the length of an exchange and off again;
it is a line per packet and is not meant to be left on.

## SOLVED: the BLE controller takes the radio from an unassociated station

The pair went deaf whenever they moved to a working channel, and six causes had
already been ruled out on the real firmware. `esp32/espnow_probe` settled it:
two boards, ESP-NOW, and nothing else, driven by hand over serial so one
variable changes at a time.

| BLE controller | station | dongle rx |
|---|---|---|
| absent (no-BT build) | never associated, ch 1 | 20 of 20 |
| absent | never associated, ch 6 | 25 of 25 |
| absent | join, leave, then ch 6 | 38 of 38 |
| up, scanning | never associated, ch 6 | **1 of 36** |
| up, scan cancelled | never associated, ch 6 | **0 of 30** |
| up, `coex wifi` preference | never associated, ch 6 | **0 of 30** |
| up, idle | **associated**, ch 1 | 24 of 24 |
| stopped and deinited | never associated, ch 6 | 25 of 25 |

**With the BLE controller running, a WiFi station that is not associated
receives nothing.** Transmission is untouched throughout -- the far board heard
36 of 36 frames from a station whose own receive count was zero, which is why
this looked for so long like the other end going quiet.

Everything else follows from it:

- the calling channel works because the station is ASSOCIATED there. Association
  is what keeps the WiFi side scheduled: it has beacons it may not miss. Leave
  the access point and the coexistence scheduler has no reason to reserve
  anything, and hands the radio to Bluetooth.
- section 23.7 requires leaving the access point. That is the whole move.
- the failure is one-sided. The M5Stack runs no Bluetooth and held up fine; the
  dongle carries a controller and went deaf. It only looked symmetric because
  the inviter sends nothing while it waits for the step-4 proof, so the M5Stack
  having nothing to receive was correct behaviour, not a second fault.
- it is not the scan. Cancelling the scan does not give the radio back. The
  controller merely being up is enough.
- `esp_coex_preference_set(ESP_COEX_PREFER_WIFI)` does not change it either.

The only thing measured to restore reception is taking the controller all the
way down (`nimble_port_stop()` then `nimble_port_deinit()`), which recovers it
immediately and completely.

### What was done about it

Bluetooth goes off the air for the length of the exchange, and comes back on the
way home. Stopping a radio for an operation that needs the other one is ordinary,
and the absence is bounded by the same local deadline that already guarantees
the station returns at all.

The order matters and is what the host test pins: the controller goes down
BEFORE the station lets go of the access point, because the moment it stops
being associated is the moment a running controller costs it every incoming
frame; and it comes back only after the reconnect has been released.

    invited X3LTSH to channel 6 (9f7dcc) -- waiting on the commons
    Bluetooth off for the exchange, heap 65468
    moved to channel 6 (radio says 6) -- deaf to the commons until we return
    working channel 6 with X3LTSH        <- 0.68 s after the move
    Bluetooth back, heap 14936
    exchange with X3LTSH ended: time is up

Ten consecutive attempts: **8 met**. Before this, the same ten-attempt run gave
one to three. More to the point, `nobody came` -- the failure this whole thread
was about, the inviter sitting alone on the working channel -- **did not happen
once**, and the step-4 proof now arrives 0.49 to 0.88 seconds after the move,
where it used to take one to five seconds when it arrived at all.

The losses were `no answer` on the commons, and two further faults came out of
chasing them.

**The invitee stayed too long.** Section 23.7 step 5 says everyone returns when
the exchange ends or `until:` passes, but `until:` is an absolute time and the
M5Stack has no clock, so it fell back to XC_DEFAULT_AWAY_MS and sat on the
working channel long after the inviter had gone home -- deaf to the commons and
to the next invitation. The invitation carries its own `ts:` as well, and
`until: - ts:` is a DURATION: two fields of one packet, subtracted, no clock
involved. Asked for twelve seconds, the clockless board now says
`moving to channel 6 for 12000ms` where it used to say 30000, and the pair comes
home within about two seconds of each other with the invitee a touch first --
which is the right way round, because its window starts when it SENDS the
acceptance and the inviter's when it HEARS it.

**One lost identity packet cost a whole attempt.** Section 23.7 is followed only
when the signature verifies, so an invitee that missed the sender's `t:identity`
ignores the invitation -- and ignores all the retries too, for the same reason,
silently. The identity was aired once and the invitation up to seven times:
measured as seven consecutive "not signed by a key we hold" on the far board
while this one recorded "no answer". They are one small packet each and they now
travel together on every attempt. That count is zero across the runs since.

Two options were considered and not taken. Refusing the move (`s:no`, which
section 23.7 already allows) costs nothing but gives up the feature on exactly
the stations that have the most to say. Never leaving the access point is not a
move at all.

### The probe

`esp32/espnow_probe`, two environments, same source:

    ~/.platformio/penv/bin/pio run -e tdongle -t upload
    ~/.platformio/penv/bin/pio run -e m5stack -t upload

Console: `ch <n>`, `join`, `leave`, `rate <ms>`, `reset`, `state`,
`ble on|off|kill`, `coex wifi|bt|balance`. Both boards print the same counter
line, so any claim about one of them can be checked against the other. Open each
serial port ONCE and leave it open -- a shell redirect to the M5Stack's CP2104
asserts DTR and reboots the board, which invalidated a whole run before it was
noticed.
