# LoRa on an XPRS station

One radio, three networks, and which one it speaks is a setting rather than
a property of the firmware. This is the LoRa reference: how the radio is
driven, how a station finds out what is on the air around it, and what it
does on each of the three protocols it can speak. The protocol side is
`spec/XPRS.md` (3.2, 9.11.5, 11.10, 14.8, 15.6); the memory arithmetic
behind the choices here is `docs/esp32.md`; the operator commands are
`docs/API.md`.

It was called `meshtastic.md` until 2026-09-21, when Meshtastic had become
one of three sections in it.

**Contents**

1. [One radio, three networks](#1-one-radio-three-networks)
2. [What every mode shares](#2-what-every-mode-shares)
3. [The channel is a setting](#3-the-channel-is-a-setting)
4. [Auto-detect: which networks are actually reachable](#4-auto-detect-which-networks-are-actually-reachable)
5. [The survey](#5-the-survey)
6. [The rules we follow](#6-the-rules-we-follow-adopted-2026-09-19)
7. [`xprs` mode: our own channel](#7-xprs-mode-our-own-channel)
8. [`meshtastic` mode](#8-meshtastic-mode)
9. [`meshcore` mode](#9-meshcore-mode)
10. [Taking turns on two networks](#10-taking-turns-on-two-networks)
11. [Configuration](#11-configuration)
12. [Memory and stack](#12-memory-and-stack)
13. [Measured on the bench](#13-measured-on-the-bench)
14. [Lessons learned](#14-lessons-learned)
15. [Not done yet](#15-not-done-yet)

## 1. One radio, four modes

A station's LoRa radio runs in one of four modes, `lora_mode`, read at start
(XPRS.md 14.8). The firmware is not tied to one LoRa network; the mode is a
setting.

| `lora_mode` | the channel | what runs on it | runs on | detail |
|---|---|---|---|---|
| `xprs` | XPRS's own: SF7 (SF9 with `[lora] profile = far`), 125 kHz, CR 4/5, preamble 8, sync 0x12; `eu` 869.5, `eu-g1` 868.2, `eu-433` 433.9, `us` 903.9, `au` 917.0 MHz | XPRS, the packet is the frame; beacons included; pace 6 s. The mode the fleet ran before 2026-09-19 and the one the P1-Pro still runs. | any board with a radio | section 7 |
| `meshtastic` (default) | Meshtastic's LongFast: SF11, 250 kHz, CR 4/5, preamble 16, sync 0x2B; `eu` 869.525, `eu-433` 433.875, `us` 906.875, `au` 919.875 MHz | XPRS inside Meshtastic frames, plus the Meshtastic repeater and bridge; only what somebody waits for; pace 10 s | any board with a radio | section 8 |
| `meshcore` | MeshCore's own, measured off a stock node: SF8, 62.5 kHz, CR 4/5, preamble 16, **sync word 0x12**, `eu` 869.618 MHz (`us` 910.525, `au` 915.8 unverified) | XPRS inside MeshCore frames (a flood-routed `RAW_CUSTOM`, which their repeaters hear but do not relay), plus the MeshCore repeater and bridge; only what somebody waits for; pace 10 s | **PSRAM only** (section 9) | section 9 |
| `both` | **the operator's own**, and this firmware blesses none: `[both] frequency`, `sf`, `bw_khz` and `sync` are required and have no defaults, because a channel two foreign networks share is whichever one somebody put them on | Meshtastic AND MeshCore at the same time: every frame classified by structure and handed to one engine or to neither, both repeaters, both bridges; XPRS itself in Meshtastic framing; pace 10 s | **PSRAM only** (section 11) | section 11 |

Three networks on one radio means three sets of foreign rules, and the file
is arranged so the shared half is read once: sections 2 to 6 are true in
every mode, sections 7 to 9 are each one network, sections 10 and 11 are the
two ways to serve two networks with one radio, and the last five are
configuration, cost, measurements and the mistakes already made.

**Two networks, three answers, and the operator gives one.** A site that
wants both foreign networks picks `rotate` (section 10: take turns, reach
both networks as they really are, about half the reception on each),
`mode = both` (section 11: serve both at once, on one channel somebody had
to put them both on), or one mode and one network done properly. None of
the three is free and the sections say what each costs.

**How it is switched, and none of it restarts the station.** The radio
retunes under the bearer's lock (`sx1262_retune`), the airtime table and the
duty ledger are rebuilt from the new mode's row, and the station keeps its
uptime, its WiFi and its other bearers:

- `cfg lora <mode>` on the serial console, which is the quickest;
- the T-Deck's Settings panel, row "LoRa mode" (OK moves to the next
  available mode);
- an owner's `t:command cmd:set lora:<mode>` (XPRS.md 11.10), answered
  `code:200 lora:<mode>` because it is true by the time the answer leaves.
  The Firmwares wapp offers it on its Name screen, and shows the running
  mode on the Stats screen;
- `config.ini` `[lora] mode = xprs`, or `cfg set lora_mode xprs`, which is
  what the station comes up in next time. Every switch above writes it, so
  a restart returns to the mode it was last put in.

What a switch costs is real and is why it stays an operator's act: whatever
was in flight on the old channel is lost, and the stations still on it stop
hearing this one. The bridge's state is NOT freed on the way out: a switch
back finds it as it was, and a 7 KB block freed and claimed again is how a
heap this size fragments (docs/esp32.md). A station that boots in `xprs`
mode allocates nothing for Meshtastic at all.

## 2. What every mode shares

Whatever the mode, these hold: listen before talk, non-blocking transmit,
one duty ledger for every network's frames, `scope:local` kept off LoRa
unless `[lora] local = yes`, and XPRS carried on every other bearer at the
same time. Stations in different modes are deaf to each other, so the mode
is a fleet decision. `/api/status` says `lora.mode`, `lora.freq_hz`,
`lora.sf` and `lora.bw_hz` (what the radio is on, not what config asked for
at boot), and a bridge's counters (`lora.mt`, `lora.mc`) only while that
bridge is the running one.

The parts of this section apply to all three modes. Anything that is true
of one network only is in its own section, 7 to 9.

### The radio

- **Listen before talk**: a header already arriving (the `HEADER_VALID` latch,
  cleared if older than 2.5 s), then SX1262 channel activity detection (two
  symbols, about 20 ms). An XPRS packet waits up to eight slots and then goes;
  a Meshtastic frame is simply retried later.
- **Transmission does not block**: `sx1262_tx_start()` returns once the frame
  is in the FIFO, and the next bearer tick puts the radio back in receive.
  The bearer task pumps every bearer, and SF11 frames are two seconds.
- **One budget**: Meshtastic frames spend the same duty ledger as XPRS
  (`xb_spend()`), and a priority frame may use the reserve.

### What a station puts on LoRa

A frame is one to two seconds on a channel two networks share, so `bridge_out`
now offers LoRa only what somebody is waiting for (`lora_worth()` in
`xprs_app.c`): messages, receipts, reactions, sos and warnings, commands and
results, identities, mailboxes, files and requests. Other stations' beacons
and service announcements stay on BLE, ESP-NOW and the LAN. The same rule
covers the LoRa digipeat and the echo carousel.

This was measured, not assumed: before it, two stations bridging a bench of
BLE and LAN beacons sat at their full 10% and the channel was busy enough
that the witness node's DM never got through.

`scope:local` (XPRS.md 9.11.1) is now enforced where it was not: it stays off
LoRa (unless `[lora] local = yes`) and off Reticulum, in `bridge_out`,
`api_send_wire` and the echo carousel. The carousel had put a Local-room
message on LoRa twenty seconds after boot.

## 3. The channel is a setting

**Every board is not an 868 MHz board.** The same SX1262 is sold matched
for 433, 868 and 915 MHz, so the frequency is set rather than assumed: `[lora] frequency` in config, `cfg freq
433.900` (or hertz) and `cfg region eu-433` on the console, the T-Deck's
"LoRa channel" Settings row, which walks the running mode's presets, and
an owner's `cmd:set freq:` / `region:` from the app. All of them are taken
at once, with the same retune a mode change uses, and the station's answer
carries `freq:` and `region:` so the owner reads back where it landed.
`freq:preset` gives the region's own channel back.

The presets each mode carries: `eu`, `eu-g1` and `eu-433` plus `us` and
`au` for XPRS's own channel; `eu`, `eu-433`, `us` and `au` for Meshtastic
(the 433 slot, 433.875, is Meshtastic's own rule applied to its EU_433
band rather than a number typed in); and for MeshCore `eu`, `us` and `au`,
because its 433 communities each pick their own channel, which is what
`freq:` is for.

**A frequency is an override, and an override is only what somebody
asked for.** Each mode carries its own channel, and the bearer uses it
unless `[lora] frequency` names one. Filling that field in from the boot
mode's region looked harmless and was not: the bearer keeps what it is
given for EVERY mode, so the boot channel followed the radio into the other
two, a live mode change never moved it and an auto-detect sweep listened to
one frequency with three modulations (found on the bench 2026-09-20).

What a station cannot answer is whether a channel is legal where it
stands. It meters against the region it was given, it says in the log when
a frequency sits outside that region's band and that the hour and the
ceiling it is still metering against were written for another one, and the
rest belongs to the operator, exactly as the power ceiling does.

**Following a channel this firmware has no preset for.** MeshCore's
presets are regional and they move: 869.618 MHz at SF8/62.5 kHz here,
SF7 on the same bandwidth in the US, and whole regions changed during
2025. `[lora] sf` and `bw_khz` set the modulation without a new firmware
(7 to 12, and 62, 125, 250 or 500 kHz; empty means the mode's own). They
are the operator's own risk and the log says what the radio was set to: a
station on another modulation is deaf to everyone on the default.

## 4. Auto-detect: which networks are actually reachable

The probe and its echo are `common/xprs_bearer_lora/lr_probe.c`, and the
sweep-and-verdict is `lr_detect.c` (host test `test_detect_host.sh`), shared
by this bearer and the nRF52 cards. A card has one radio and no `both` mode,
so it uses the verdict to LIVE on one network: at boot the best of the two
(a probe carried back, then an XPRS station heard, then any frame), every
hour the other network and its own again, and a move only after two sweeps
in a row agree.

`cfg detect [seconds]`, the T-Deck's "LoRa auto-detect" row, or
`[lora] detect_s`. It walks the modes like the survey below, but on each
mesh mode it ASKS: one small packet of the kind that network floods, and a
repeater within reach answers by re-airing it. Hearing our own packet come
back with a hop on it is proof of a working relay rather than a guess.

**Why asking is necessary.** Listening alone cannot answer "is anybody
there", at any dwell a person would wait through: a MeshCore node
advertises every one to four hours (`advert.interval`, default 2) and a
Meshtastic node sends a NodeInfo about every three. A channel with a live
repeater on it is silent for almost all of that time. What IS quick is the
answer to a packet, and each network's own contention rule says how quick.

**Why each network gets its own wait.** The rules differ by an order of
magnitude, so one flat figure is either wasteful or wrong:

| mode | the arithmetic | ceiling |
|---|---|---|
| `meshcore` | 6 B at SF8 / 62.5 kHz is 177 ms on air; the flood wait is `rand(0..5) * (airtime * 52 / 50) / 2`, at most 461 ms. An answer is back inside about a second and a half, CAD retries included. | 8 s |
| `meshtastic` | 22 B at SF11 / 250 kHz is 436 ms on air; the CLIENT rule waits `2 * 8 * slot + rand(0..2^cw) * slot` with `cw = 3 + (snr + 20) * 5 / 30`, and the STRONGEST signal draws the LARGEST window. A faint repeater answers in 644 ms; the close one that matters most can take 7.6 s. | 20 s |
| `xprs` | nothing is asked: our own stations beacon every few seconds, so this is pure listening. | 10 s |

That inversion in the Meshtastic rule is the whole reason the answer to
"can you find a repeater in under ten seconds for each type" is no for
that network. MeshCore, yes, comfortably. Meshtastic, only by giving up
exactly the neighbours a station most wants to find.

**And each mode ends the moment a repeater carries the probe.** The
ceilings above are worst cases, not the usual cost. Measured on the bench
on 2026-09-20: a station in Meshtastic mode relayed the probe and the
sweep left that mode after 6.1 s; a stock MeshCore repeater (v1.17.1,
869.6179809 MHz) relayed it and the sweep left after 2.0 s. Only a quiet
network spends its full ceiling, which is right: silence is the case that
needs the waiting.

Hearing somebody ELSE's frame does NOT end the dwell, although it is
counted and reported. The first version ended on any frame, and on the
bench a stray Meshtastic frame arrived 3.5 s in and cut the dwell off
while the repeater was still inside its own backoff: the sweep threw away
the very answer it had asked for. The ceilings exist for that backoff, so
only the answer may shorten them.

**Two things the bench changed in the probe itself**, both of them
invisible in a host test:

- It goes out 1.2 s after the retune, not immediately. A probe aired
  about 30 ms after `sx1262_retune` was transmitted in full (the radio
  reported TX_DONE after its exact airtime) and no neighbour ever
  demodulated it, four runs out of four. A second probe goes out halfway
  through the dwell, because one packet can always meet another.
- The Meshtastic probe rides on the LongFast channel hash, not on XPRS's.
  On the XPRS channel a station running this firmware unwraps it as a
  piece of an XPRS wire, parks the piece that never completes, and so
  never repeats it: every XPRS neighbour was deaf to the probe while
  stock routers would have carried it. The portnum stays private, so
  nobody reads it.

`[lora] detect_s` overrides all three with one figure (5 to 300) for an
operator who wants to say so; left empty, each network keeps its own. A
`cfg survey` is a different thing, listen-only, and keeps its fixed window
on every mode because it is measuring how busy a channel is, not asking a
yes or no.

**What it puts on the air**, once per mesh mode:

| mode | probe | why that one |
|---|---|---|
| `xprs` | nothing | our own stations beacon every few seconds; listening is enough |
| `meshtastic` | a Data frame on the LongFast channel, private portnum, one byte | routers relay by the header, not by what they can read; on XPRS's own channel our own stations swallow it as half a wire (see above) |
| `meshcore` | an ACK with a checksum that matches nothing, four bytes | no crypto at all, and a type their repeaters carry (proven against a stock v1.17.1 repeater on 2026-09-20); it means nothing to anybody, so nothing acts on it |

No signature and no key exchange: this runs on the bearer's task between
retunes, where there is no room for curve arithmetic (docs/esp32.md).
The airtime it costs is charged to the region's hour when the sweep ends,
because a probe is a transmission like any other.

`/api/status` carries `asked` and `relayed` per mode beside the frames and
the names.

## 5. The survey

Auto-detect asks a question; the survey measures a channel. `cfg survey
[seconds]` (or the Settings row, or
`[lora] survey_s`, default 60): the station listens on each available mode
in turn, transmits nothing while it does, hands nothing to the bearer or to
a bridge, and then goes back to the mode it started in and says what it
heard. It names what it can: XPRS callsigns from the wires, Meshtastic nodes
from their NodeInfo, MeshCore nodes from their adverts (the one MeshCore
frame that is signed, so a name read from one is a name somebody stands
behind). `/api/status` carries the last one under `lora.survey`:

```
survey: listening 25s on each mode, starting with xprs
survey: xprs 7 frames, 3 named -- X3DCK0
survey: meshtastic 0 frames, 0 named
survey: done, back in xprs mode
```

That is the answer an operator needs before choosing a mode, measured rather
than guessed. Use auto-detect for "is anybody there", the survey for "how
busy is it": the survey keeps its fixed window on every mode, because a
frame count means nothing if the window moves.

## 6. The rules we follow (adopted 2026-09-19)

These are decisions, not descriptions. Code that breaks one of them is a
regression even if it works on the bench; each one has already cost a bug.
The app half of the same rules is in `app/docs/meshtastic.md` section 5.

**Addressing and reach**

1. **A Meshtastic node is `MT` plus its node number in eight uppercase hex
   digits**, and a MeshCore node `MC` plus the first four bytes of its
   public key. Never an `X` class. Every surface
   asks one rule what an address names: `xprs_is_foreign_call` in the
   firmware, `xprsKindOf`/`xprsAddressKind` in the app, handed to wapps as
   `kind` and through `hal_xprs_kind`. No wapp tests a prefix.
2. **A Meshtastic user can write only to XPRS callsigns a bridge has
   announced**, that is, callsigns whose words have crossed onto Meshtastic.
   There is no directory and nothing to type.
3. **The sender's consent sets the reach.** Without "OK to MQTT" (the
   Meshtastic app's default) a translation is `scope:local`: this station's
   local bearers now, never the internet, never carried for someone absent,
   never deposited with an archiver (XPRS.md 13.11.3). The station's own index
   keeps it. With "OK to MQTT" it is global and goes onto Reticulum too.
4. **A DM goes onto LoRa only for a Meshtastic node this bridge has heard
   itself** (in its node table since boot, or its key, learned here and
   kept), or when one of this station's own users handed it over
   (`MT_XPRS_OWN`: its screen, its API, a phone talking to it). A reply sent
   from far away reaches every bridge on the internet, and without this each
   of them would air it and ask for the key of a node that is nowhere near.
   The bridge that hears the node delivers; the others count it
   (`dm_not_here`) and stay quiet.
5. **Nothing to another network is sealed.** The app refuses to seal to a
   foreign callsign (`XprsSealRefusal.foreignNetwork`, `hal_xprs_message`
   returns -3) and the bridge refuses a sealed body out loud (`s:no`).

**Keys and identity**

6. **An XPRS callsign's Meshtastic key is derived from the callsign**, so
   every bridge presents the same one. Meshtastic keeps the first key it
   hears for a node; two bridges with two keys would break every DM.
7. **A bridge keeps what it learned across a restart**: Meshtastic keys
   (`xprsmt`/`keys`) and the callsigns it has announced (`xprsmt`/`vnodes`),
   and nothing else. A flash write stops both cores' cache; a write per
   station heard would be one a minute on a busy bench.

**Translation**

8. **A translation is the gateway's own packet**: unsigned, `via:` naming the
   gateway, dated to the minute, carrying `zmid:`. It leaves through the
   station's own send path (`mesh_out`), never the relay path, which refuses
   a wire whose `via:` already names this station.
9. **Two gateways, one message.** The section 5 identifier is the first
   duplicate key and `zmid:` (with the part number) the second, checked once,
   at the app's one receive door (`PacketGateway`), never in a courier or a
   wapp.
10. **What came from Meshtastic never goes back**, and XPRS older than 30
    minutes (a broadcast) or 6 hours (a DM) is not translated.
10a. **Every gateway in earshot translates a BROADCAST; only one delivers a
    DM.** A reader on a foreign network may be in range of just one of the
    gateways, so each announces a broadcast as its own packet with its own
    `via:` (rule 8), and the rate is held by `broadcasts_per_hour`. A direct
    message is the opposite case and rule 4 governs it: the bridge that hears
    the recipient delivers, the others count `dm_not_here` and stay quiet.
    Measured on the bench 2026-10-03 with two gateways on one channel: one
    phone broadcast, `text_out` +1 on each station for each network. That is
    not the duplicate rule failing. What IS deduplicated is the repeat --
    several stations hear one frame and one re-airs it.

**The radio and the tasks**

11. **On Meshtastic's channel, LoRa carries only what somebody waits for**
    (`lora_worth`): messages, receipts, reactions, sos, warnings, commands,
    results, identities, mailboxes, files, requests. Presence stays on the
    cheap bearers. On XPRS's own channel (`xprs` mode) it carries everything,
    as it always did there.

    **The exception: a carried card** (`models/sensecap-t1000e`, adopted
    2026-10-07). A station a person carries has no other way to say where
    that person is, so it airs a LEAN beacon on LoRa: `pos: acc: temp: batt:
    mail:` and the most recent few of `hears:`, no `volt:`, every fifteen
    minutes (an hour when it is still). It pays for that by being stricter
    than rule 11 in the other direction: from Bluetooth it carries no packet
    for somebody heard in the same room, no replay more than half an hour
    old, nothing twice in half an hour, station-to-station commands and
    results only toward a station heard on LoRa, and a phone's identity
    once in three hours. On the bench that was three frames where rule 11
    alone allowed thirty-eight.
12. **Nothing slow runs on the LoRa task or under the bridge's mutex.** The
    curve work runs on the bearer tick; translations and receipts are parked
    by `mesh_deliver` and sent from `idx_task` on core 1. Every other
    bearer's task (the Bluetooth host among them) waits on that mutex to offer
    the bridge a packet. Deciding WHICH network a received frame belongs to
    (`lr_class.c`, section 11) is arithmetic over its bytes and, at most, one
    AES-128 pass over its payload, which is the same work `survey_frame`
    already does here. A signature is never a classification test: an
    Ed25519 verify is 3.9 KB of stack and runs on `mcwork` alone, after the
    frame has already been handed to MeshCore.
13. **A DM retried after the recipient could not open it goes under a new
    id.** A Meshtastic node records an id as seen even when it fails to
    decrypt, and drops every repeat of it silently.
14. **Counted where it can be read.** `/api/status` carries the bridge's
    counters under `lora.mt`; a new behaviour gets a counter there the day
    it is written.
15. **A bridge is silent unless the running mode serves its network.**
    Every door the bridge has (its transmit hook, its tick, the frames
    handed to it, the packets offered to it, and the counters it reports)
    asks the bearer WHICH NETWORKS THE RUNNING MODE SERVES (`lr_serves`),
    never which mode it is. A station switched to `xprs` aired a LongFast
    frame seconds later on the first try, because only the receive path had
    been gated. A mode that serves two networks is this rule with two
    answers rather than an exemption from it: in `both` mode both doors are
    open because both networks are on the channel, and in every other mode
    at most one is.
16. **The LoRa network is a setting, never an assumption.** Code that only
    makes sense on one channel (the Meshtastic bridge, `lora_worth`, the
    framing) asks the running mode (`xprslora_mode()`); a mode's radio
    profile, regions and pace live in one row of `k_modes` in
    `xprslora.c`, and a new network (MeshCore) is a new row, a new word in
    `xsetup_check`, and its engine behind the same `mesh` flag. A mode may
    serve MORE THAN ONE network; then the row says which (`net =
    LR_NET_BOTH`) and one function says which framing XPRS's own traffic
    wears (`lr_xprs_net`). And a mode whose channel is the operator's own
    carries no channel numbers in its row at all: it refuses to start until
    configuration supplies the frequency, the spreading factor, the
    bandwidth and the sync word. **No shared channel is blessed in this
    firmware, because there is no shared channel to bless** -- it is
    whichever one the operator put both networks on.

## 7. `xprs` mode: our own channel

The mode the fleet ran before 2026-09-19, and the one the SenseCAP P1-Pro
still runs. XPRS's own channel, nobody else's: SF7 (SF9 with
`[lora] profile = far`), 125 kHz, CR 4/5, preamble 8, sync word 0x12.
Presets `eu` 869.5, `eu-g1` 868.2, `eu-433` 433.9, `us` 903.9 and `au`
917.0 MHz.

What is different from the two mesh modes, and it is most of the file:

- **The packet is the frame.** No wrapping, no fragment marker, no
  translation: an XPRS wire goes on the air as itself and `xprs_looks_like`
  is the whole receiver.
- **It carries everything**, beacons and observations included, because the
  channel is not shared with another network's users. `lora_worth` applies
  to the mesh modes only. The pace is 6 s rather than 10.
- **Nothing is allocated.** No bridge, no repeater state, no worker task: a
  station that boots in `xprs` mode is the cheapest LoRa station this
  firmware makes (the Heltec V3 keeps 18.9 KB free against 11.8 KB with the
  Meshtastic bridge running).
- **Relaying is XPRS's own**, by the `via:` chain and the duplicate rules in
  XPRS.md 9.2, not by a foreign network's hop counter.
- **Auto-detect only listens here**, because our own stations beacon every
  few seconds: a channel with a station on it answers by itself inside the
  ten-second ceiling, and there is nothing to ask.

Use it when the stations that matter are all XPRS, when the band is
crowded and SF7 buys back the airtime SF11 spends, or on a board that
cannot afford a bridge. Stations in different modes are deaf to each
other, so it is a fleet decision, not a per-station one: moving the ESP32s
to LongFast in 2026-09 cut off the P1-Pro until its mode was set back.

## 8. `meshtastic` mode

The default. XPRS rides inside Meshtastic frames on LongFast, the station
repeats Meshtastic traffic by Meshtastic's own rules, and the two networks'
users can write to each other. Everything in this section is that mode
only.

### What it does

Since 2026-09-19 every XPRS station with a LoRa radio runs, by default, on
Meshtastic's default channel and does three things there at once:

1. carries XPRS, as before, inside frames of its own;
2. **repeats** Meshtastic traffic by Meshtastic's own flood rules;
3. **bridges**: Meshtastic users and XPRS users message each other, on the
   public channel and directly, both ways.

The code is `common/xprs_meshtastic` (platform-free, host-tested) and
`common/xprs_bearer_lora` (the radio). The protocol side is in
`spec/XPRS.md` sections 3.2, 9.11.5 and 14.8. The earlier evaluation that
decided against this lives in `app/docs/meshtastic.md`.

Meshtastic's firmware and `.proto` files are GPL-3.0 and this tree is
Apache-2.0. Nothing of theirs is copied or linked: the header, the few
protobuf messages, the channel arithmetic, X25519 and AES-CCM are written from
the public field numbers and the RFCs, and checked against Meshtastic's Python
package and the `cryptography` library in the host test.

### The channel

| | |
|---|---|
| modulation | LongFast: SF11, 250 kHz, CR 4/5, preamble 16, explicit header, CRC on |
| sync word | `0x2B` (register `0x0740` = `0x24B4`) |
| frequency | Meshtastic's slot rule for the channel name `LongFast`: `eu` 869.525 MHz, `us` 906.875 MHz, `au` 919.875 MHz |
| budget | `eu`: 360 s of airtime an hour (10%), 21 s reserved for sos/warning; `us`/`au`: none, and no dwell cap (see below) |

`lora_region` now takes `eu`, `us` or `au`; `eu-g1` (868.2 MHz) is gone,
because Meshtastic has no channel there. `lora_freq_hz` still overrides the
frequency, and the station warns that no Meshtastic node will hear it.
`lora_profile` (`far`, SF9) is gone: one channel, one modulation.

**The US and AU rows used to cap one transmission at 400 ms.** No SF11 frame
fits that, and Meshtastic applies no such cap. Whether one binds a given
installation is the operator's question; `lora_duty_ms`/`lora_resv_ms` still
set a budget.

### XPRS on the shared channel

An XPRS wire is the payload of a clear Meshtastic `Data` on private portnum
`0x158` (256 + 'X'), channel `XPRS` with no key (hash `0x09`). Every
Meshtastic node ignores it cleanly and relays it (tested: a stock node
relayed our frames and they arrived intact). A bare XPRS wire would have been
read as a garbage header and relayed with bytes 12 to 15 rewritten.

- One frame carries 233 bytes of XPRS; 234 to 250 go as two frames with a
  3-byte fragment marker, joined again within 15 s.
- The header is derived from the packet: `from` = the node number of `f:`,
  `id` = the first four bytes of the section 5 hash. Two stations airing the
  same packet air the same (from, id), and every duplicate filter agrees.
- Meshtastic's `hop_limit` is 3 minus the `via:` count, and only for what
  somebody waits for (message, sos, warning, receipt, reaction, command,
  result, file, mailbox). Beacons go out with 0, so Meshtastic routers leave
  them alone.

### Identities

- A Meshtastic node is `MT` + its node number in eight uppercase hex digits:
  `MT0C39F654`.
- An XPRS callsign is a Meshtastic node whose number is the first four bytes
  of `sha256("XPRS/node" || bare callsign)`, moved off 0..3 and the broadcast
  address. The station's own callsign is its node; a bridge also speaks for
  every XPRS callsign it hears, as a virtual node.
- Each XPRS node announces a NodeInfo: long name `nick CALLSIGN` (the
  callsign is always there, because it is the address and the mark another
  bridge recognises), short name the last four characters, hardware
  `PRIVATE_HW`, and a public key.
- **The key is derived from the callsign**: private =
  `sha256("XPRS/mt/x25519" || bare callsign)`, clamped. Every bridge presents
  the same key for a callsign, which it must, because Meshtastic locks in the
  first key it hears for a node. The price, decided with the user: a DM to an
  XPRS callsign is readable by anyone who knows the rule. That is the privacy
  of the public channel, and the bridge turns it into clear XPRS anyway.
  Meshtastic apps still show these DMs with a lock.

### Direct messages

Meshtastic 2.5 and later refuse to send a DM on the channel key ("refusing to
send legacy DM") and reject one on receipt ("Rejecting legacy DM"). A DM is
X25519 between the two nodes' keys, SHA-256 of the shared secret as the
AES-256 key, and AES-CCM (8-byte tag, 13-byte nonce: id, a random 32-bit
extra nonce, from) on channel hash 0. `mt_pki.c` does it; the bridge keeps
the keys it learns from NodeInfo in NVS (`xprsmt`/`keys`), because the
firmware answers a NodeInfo REQUEST from one node only once in 12 hours.

- **Meshtastic to XPRS.** A DM to an XPRS node is opened with the node's
  derived key and becomes `t:message f:MT… d:<callsign>`, and the bridge acks
  it. When it does not know the sender's key it answers `PKI_UNKNOWN_PUBKEY`,
  as the firmware does; the sender sends its NodeInfo at once and its user
  sees why the DM failed.
- **XPRS to Meshtastic.** `t:message d:MT…` is sealed to the node's key and
  sent from the XPRS author's node with want_ack. Without the key the bridge
  asks for the node's NodeInfo and holds the DM. The node's ack becomes a
  signed `t:receipt f:<bridge> r:<id> s:ack` (a refusal: `s:no m:Meshtastic
  said <n>`). Retried three times, then held until the node is heard again,
  for up to a day.
- **When the node cannot open it.** A node that has no key for our node
  answers `PKI_UNKNOWN_PUBKEY`, and it has already recorded the DM's id as
  seen, so a retry under the same id is dropped without a word. The bridge
  sends that node our NodeInfo, then sends the DM once more under a new id
  (derived from the old one, so every bridge picks the same), and only a
  second 35 becomes `s:no`. A node's first DM also waits
  `MT_AFTER_NODEINFO_MS` behind the NodeInfo queued for it, so the key
  normally arrives first.
- **A translation leaves as the gateway's own packet.** Its `via:` names
  the gateway (XPRS.md 9.11.5), and the relay path refuses a wire whose
  `via:` already names this station, so `mesh_out` in `xprs_app.c` sends it
  the way the station sends its own: LAN, ESP-NOW and Bluetooth, the
  internet only when not `scope:local`, never onto LoRa as XPRS. It is
  parked by `mesh_deliver` and sent from `idx_task` (core 1), like a gateway
  receipt: the translation is made under the bridge's mutex on the LoRa
  task, and a Reticulum send can wait seconds, which every other bearer's
  task (the Bluetooth host among them) would spend waiting on that mutex.
- **The callsigns a bridge has announced survive a restart** (NVS
  `xprsmt`/`vnodes`). A node number cannot be turned back into a callsign,
  and Meshtastic keeps every node it has heard, so without this a DM from a
  Meshtastic user to an XPRS station was flooded past the bridge until that
  station happened to speak again. Only announced ones are saved (those are
  the nodes a Meshtastic user can see and write to), so a flash write
  happens when an XPRS user first appears on Meshtastic, not whenever a new
  station is heard; and announced ones are the last to be evicted from the
  table.
- **Only nodes heard here** (rule 4): a DM that reached this station from
  somewhere else goes onto LoRa only when the node is in this bridge's node
  table or key table; otherwise it is counted in `dm_not_here` and left. A
  DM from one of this station's own users (`MT_XPRS_OWN`) may still ask an
  unheard node for its key. The counter counts copies: one DM heard over the
  LAN, Bluetooth and the echo carousel counts each time.
- **Acks follow the firmware's rule** (ReliableRouter): a packet to one of our
  nodes that wants an ack gets one, and an ack or reply that itself wants one
  (a DM's ack does) gets a zero-hop ack when it came straight from its sender.
- A sealed XPRS body cannot cross and is refused out loud to the station that
  handed it over.

### Broadcasts, replies, likes, names

- LongFast text becomes `t:message f:MT…` with no `d:`; `scope:local` unless
  the sender set Meshtastic's "OK to MQTT" bit.
- An XPRS broadcast `t:message` (not sealed, not local) is mirrored onto
  LongFast from the author's node, at most `mt_bcast_hr` (12) an hour per
  station.
- Replies keep their thread both ways (`r:` and `reply_id`), through a ring
  of recent pairs. A thumb or a heart tapback is `add:like`, and back.
- A NodeInfo becomes an unsigned `t:identity f:MT… nick:…`, at most every six
  hours per node unless the name changes.
- Every translation carries `via:<bridge>` and `zmid:<from><id>` and is dated
  to the minute, so two bridges hearing the same frame compose the same packet.
- What came from Meshtastic is never sent back to it. An XPRS packet already
  translated is not translated again, and one older than 30 minutes (a
  broadcast) or 6 hours (a DM) is not translated at all: echoes and history
  replays keep old packets moving, and after a restart the bridge cannot tell
  old from new. Meshtastic's duplicate filter hides a repeat from its users
  (the ids are derived), but not from the channel.

### The repeater

A frame not heard before (dedup on from and id, ten minutes), with hops left,
not ours and not for one of our nodes, and whose `next_hop` is 0 or ours, is
re-aired with `hop_limit - 1` and our relay byte, after the firmware's CLIENT
wait: `2 x 8 x slot + random(0, 2^cw) x slot`, the slot 28 ms at LongFast and
`cw` 3 to 8 from the SNR, so a faint copy goes first. Hearing somebody else
relay it cancels ours. Frames are relayed whether or not they can be read.

## 9. `meshcore` mode

The same three jobs on MeshCore's channel: XPRS inside a flood `RAW_CUSTOM`
(direct range only, because a stock repeater does not carry that type), the
MeshCore repeater, and a bridge with signed adverts and direct messages.
Needs PSRAM. Everything in this section is that mode only.

### What it does

`meshcore` mode puts the same XPRS traffic on MeshCore's channel. The code is
`common/xprs_meshcore` (platform-free, host-tested, `test_mc_host.sh`), a
sibling of `common/xprs_meshtastic` and built on the same shared primitives
(`common/xprs_loracrypto`).

MeshCore's firmware is MIT and this tree is Apache-2.0. Nothing of theirs is
copied or linked either: the frame, the payload layouts and the crypto are
written from its published format (docs.meshcore.io `packet_format` and
`payloads`, and the field names in `Packet.h`, `Identity.cpp`, `Utils.cpp`)
and checked in the host test against OpenSSL and against the byte layouts
themselves.

**The frame.** `[header][transport codes (4, optional)][path length][path]
[payload]`. The header is one byte, `0bVVPPPPRR`: route type in bits 0-1,
payload type in bits 2-5, version in bits 6-7. The path length byte carries
the hop count in bits 0-5 and each hop hash's size, minus one, in bits 6-7. A
payload is at most 184 bytes. A repeater appends its own hash to the path and
re-airs; the identifier everybody dedups on is a hash of the payload and the
type, so the path growing does not make it a different packet.

**What XPRS puts there** is `RAW_CUSTOM` (0x0F), flood-routed, which is
MeshCore's own answer to a payload a node does not understand, exactly as a
private portnum is on Meshtastic. One marker byte in front of the wire: 0x00
for the whole of it, or `0x80 | part` and a 16-bit tag for the two halves of
a wire past 183 bytes. The tag is derived from the packet's own identifier
(XPRS.md 5), so two stations airing one packet air identical bytes and every
duplicate filter on the channel agrees.

**Identities.** A MeshCore node is addressed by the FIRST BYTE of its Ed25519
public key, and wears the callsign `MC` plus the first four bytes of that key
in uppercase hex. An XPRS callsign's MeshCore identity is derived from the
callsign (`sha256("XPRS/mc/ed25519" || CALLSIGN)`), so every bridge presents
the same one and any of them can carry that callsign's mail. None of those
keys is secret, and that is the point.

**The crypto**, all of it MeshCore's, none of it ours: a direct message is
`dest(1) src(1) MAC(2) ciphertext` under AES-128-ECB with the first sixteen
bytes of the ECDH secret between the two identities, the MAC being the first
two bytes of HMAC-SHA256 over the ciphertext; a public-channel message is
`channel hash(1) MAC(2) ciphertext` under the channel key, the hash being the
first byte of its SHA-256, the body `<sender name>: <text>` with the sender
unauthenticated; an advert is `key(32) timestamp(4) signature(64) appdata`,
signed Ed25519, and is the only frame whose name is worth anything.

**The repeater and the bridge** are `mc_mesh.c`, and they follow the rules
above ("The rules we follow") without exception: `via:` names the gateway,
translations are unsigned and dated to the minute, `zmid:` carries
MeshCore's own identifier (the packet hash), nothing goes back, a sealed
body never crosses, and a direct message is put on the air only for a node
this bridge has heard. Config is `[meshcore]` (`repeat`, `bridge`,
`broadcasts_per_hour`, `advert_min`), the counters are `lora.mc` in
`/api/status`, and `serve:meshcore` rides the beacon while that bridge is
the running one.

The repeater re-airs a flood packet with its own hash appended to the path
after MeshCore's own wait, `rand(0..5) * (airtime * 52 / 50) / 2`, and drops
its copy when somebody else airs the packet first; a direct-routed packet is
carried only by the node whose hash is at the front of the path, which takes
itself off it. Everything the bridge airs is DETERMINISTIC (AES-ECB has no
nonce, the timestamp is the XPRS packet's own, a callsign's key is derived
from the callsign), so two bridges translating one packet produce identical
bytes, the same packet hash, and cancel each other.

**Adverts are spaced.** A station speaks for every XPRS callsign whose
words it mirrors, and each one has to advertise before a MeshCore user can
see it, so a busy minute could put a dozen adverts on a shared channel at
once (eight in five minutes from ordinary bench traffic). They go one
every thirty seconds instead (`MC_ADVERT_GAP_MS`), and none is dropped:
each keeps its turn. The exception is a node somebody is writing to right
now, whose advert goes ahead of the queue, because a direct message that
overtakes its own key is unreadable.

**What one message carries.** 171 bytes of text (`MC_TEXT_MAX`), the
sender's name included on a channel message; longer is shortened on the
way out, as it is on the Meshtastic side, because the whole of it is on
XPRS where the words were said. A message arriving from MeshCore is split
over XPRS parts instead, which XPRS has a grammar for (7.6).

**A board without PSRAM cannot run this mode**, and says so rather than
trying: the state and the worker's stack come to about thirteen kilobytes
against the eleven the Heltec V3 has free with the Meshtastic bridge
running, and the arithmetic does not close (docs/esp32.md, "The arithmetic
that did not close"). Such a board refuses the switch and stays where it
is; at boot it comes up in its default mode with a line saying why, and
the setting is left alone for a board with room. Measured the hard way:
the first version checked the free heap at claim time, passed with 48 KB,
and reboot-looped with 1,920 bytes.

**What it costs, and where the work runs.** The whole of `meshcore` mode
(the bridge, the contact table and the reassembly of split XPRS wires) is
one block: 10,064 bytes on a board with PSRAM, 6,792 on one without, where
the tables shrink as Meshtastic's do. It is claimed on the way into the
mode and, on internal RAM only, released on the way out, because a board
without PSRAM cannot hold this and the Meshtastic bridge at once. In PSRAM
it is kept, like the Meshtastic bridge's. If the claim fails, the station
stays in the mode it is in and says so in the log: never a silent fallback
onto a channel it cannot read.

On top of that the bridge has a TASK of its own, `mcwork`, 6 KB of stack on
core 1, started with the bridge and stood down when the mode is left or an
install needs the room. It exists because MeshCore signs its adverts:
reading one is an Ed25519 verification, 3.9 KB of stack measured on the
target compiler, and the bearer task has about two to spare
(docs/esp32.md, "Task stacks are heap"). So the bridge is split. The
receive path parks the payload and does no arithmetic; `mc_mesh_work` does
every signature, key exchange and derived key, and the NVS writes with
them; `mc_mesh_tick`, on the bearer task, only puts what is due on the air.
An XPRS packet handed in from another bearer (`mc_mesh_on_xprs`, which may
arrive on the Bluetooth host's task) derives nothing either: a virtual
node has no MeshCore address until the worker has given it one.

**Two things MeshCore does not give us, and the bridge does not pretend
otherwise:**

- A channel message is **not signed** and names its sender only by a NAME.
  A name is not an address, so a channel message crosses only when that name
  matches exactly one node whose advert this station has heard, and it
  crosses under that node's address. Everything else is counted
  (`mc.unnamed`) and dropped: inventing an address from a name would put
  words in the mouth of a node that may not exist.
- A node is **addressed by the first byte of its key**, so opening a direct
  message means trying the contacts whose key starts with that byte, and we
  only try when the byte is one of ours. There is no asking MeshCore for a
  key either: a contact whose advert we never heard is a contact we cannot
  write to at all, which is the "only where the node is" rule arriving for
  free.

**What is not there.** Reactions (MeshCore has no tapback and no reply
field, so a like would arrive as a line of its own), positions and
telemetry, and MeshCore's room servers and transport routes, which a
repeater does not need.

## 10. Taking turns on two networks

One radio, two networks, and an operator who wants both: `[lora] rotate =
meshtastic,meshcore` makes the station serve them in turn. It is a real
feature and a real compromise, and the compromise is the important half,
so it is stated first.

**While the station is on one network it is deaf to the other.** That is
not an implementation detail to be improved later: a LoRa receiver hears
one modulation and one sync word, and there is only one of it.

That sentence is why section 11 exists and is not a contradiction of it but
a consequence. The only way one receiver hears two networks at once is for
both networks to have been put on one modulation and one sync word, and that
is work on their nodes, not in this firmware. A stock Meshtastic node and a
stock MeshCore node are on different frequencies with different modulations
and different sync words, and **no mode of this radio hears both of them**.

### What the other networks keep for a node that was not listening

Almost nothing, and never by default. Researched 2026-09-21 against both
firmwares' own sources:

| | Meshtastic | MeshCore |
|---|---|---|
| the one mechanism | the Store & Forward module | the room server |
| who runs it | an ESP32 **with PSRAM**, `enabled` off by default, router role | a node flashed as `room_server` |
| what it holds | text it could DECRYPT. Since public-key DMs became the default it cannot read, and so cannot store, a direct message between two other nodes | 32 posts (`MAX_UNSYNCED_POSTS`), in RAM, lost on reboot |
| how it is collected | `CLIENT_HISTORY` with a window in minutes; at most 25 messages (`historyReturnMax`) from the last 240 (`historyReturnWindow`), one per 5 s, sent `want_ack = false`, and **refused on the default channel** | a login (`ANON_REQ`) carrying `sync_since`; the server pushes one post per 1.2 s until each is acknowledged |
| the repeaters | stateless flood, no queue for anybody | stateless flood, no mailbox anywhere |

So a rotating station cannot ask either network to hold its mail. What it
can rely on is the sender's own patience, and that is short.

### The numbers the dwell is chosen from

| | Meshtastic | MeshCore |
|---|---|---|
| direct message | 3 airings (`NUM_RELIABLE_RETX`), `2*airtime + CW + 4500 ms` apart: 7-8 s idle, ~15 s busy. **All of it inside 15-45 s**, then the sender NAKs | the firmware never retries (`onSendTimeout(){}`); the client app does, 3 attempts, `500 + 16*airtime` flood / `500 + (6*airtime + 250)*hops` direct: about 6-8 s each on our channel, so **20-25 s** |
| broadcast / channel message | aired **once**, no retry | aired **once**, no ACK, no retry |
| dedup | a ring of ids with **no clock** (160 to 500 of them) | a ring of 160 hashes, **no clock**, persisted across reboot |

Two things follow, and they are the whole design:

1. **Nothing punishes us for having been away.** Neither dedup table
   expires by time, so a retry that arrives after we come back is new to
   us and is accepted. A station does not have to be present when a
   message is FIRST sent, only while one of its attempts is in the air.
2. **A slice longer than a sender's patience loses the message.** A
   minute away is longer than either network's entire retry budget.

### The rules, and the number behind each one

The decision is `lr_rotate_due()` in `common/xprs_bearer_lora/lr_rotate.c`,
kept apart from the radio so it can be tested (`test_rotate_host.sh`).

| rule | why |
|---|---|
| never leave before `rotate_s` (default **25 s**) | shorter and the slice is mostly the 1.2 s settle after the retune, and each network sees a station that appears and vanishes |
| stay while the running bridge is busy: a frame due, a direct message inside its retry budget, or a DM either way in the last 60 s (`mt_mesh_busy`, `mc_mesh_busy`) | their sender gives up in 15-25 s, so walking out of an exchange loses it |
| but leave anyway after 4 slices (100 s) | on a lively channel "busy" is nearly always true, and the other network would never be served again |
| never mid-transmission, and never inside 1.2 s of a retune (`LR_SETTLE_MS`) | a frame aired ~30 ms after `sx1262_retune` was transmitted in full and demodulated by nobody, four runs out of four (2026-09-20) |
| channel chatter does **not** count as busy | a public channel is never quiet; counting it would starve the other network |

A packet's two frames cannot be split by a turn: `lr_air` puts both on the
air inside one call, on the same task the scheduler runs on.

### What it costs, measured

Measured 2026-09-21. A Heltec V3 in `meshtastic` mode aired one numbered
packet every 12 s on LongFast (`/api/xprs/send` with `bearer: lora`, so
the cadence is ours and not the bench's); a T-Deck two metres away
counted what arrived, first standing still on that network and then
rotating between the two.

| | aired | arrived | |
|---|---|---|---|
| standing still on Meshtastic | 20 | 16 | 80 %, the bench's own baseline: the rest went to a busy channel and a spent budget |
| rotating, 25 s slices | 20 | 8 | 40 % |

A rotating station heard **half of what a still one did**, which is the
time it spent there and nothing worse: the switching itself cost no
measurable extra loss. Halve the reception, roughly, for each extra
network in the ring.

The other half of the cost is the hour. A rotating station spends ONE
regional allowance on two networks (both of these channels are in the
same EU sub-band), so the budget drains about twice as fast: in the same
run `lora.spent_ms` rose 13.0 s to 54.7 s in five minutes, against a
360 s hourly allowance. Watch `lora.free_ms`, not just the counters.

And the ledger holds across a turn: 11 turns in five minutes, and
`spent_ms` never once stepped backwards. It used to, before
`xb_set_duty_keep` (see "Lessons learned").

**A stock repeater serves a rotating station normally, on its turns.**
Against a Heltec V3 running the published MeshCore v1.17.1 repeater build
(869.6179809 MHz), a rotating T-Deck mirroring XPRS broadcasts onto
MeshCore's public channel, 2026-09-21:

| | |
|---|---|
| our channel message out, on a MeshCore turn | `mc tx type 05 route 1 hop 0 37B` |
| the repeater carrying it, about a second later | `mc type 05 route 1 hop 1 38B -29 dBm` -- one byte longer, its hash appended to the path |
| every MeshCore turn in the run | the same, four for four |
| the repeater's own advert | heard during a turn (`mc type 04 route 2`) |
| during Meshtastic's turns | nothing on that channel at all, which is the rotation doing what it says |

So the repeater does not treat an intermittent neighbour differently --
there is no session to lose, which is the one advantage of a network that
holds nothing for anybody. What is lost is only what was aired while the
radio was elsewhere.

**Half an hour unattended, T-Deck, 2026-09-21.** Turned on through
`[lora] rotate` and a restart, which is the path a deployed station uses,
and left alone:

| | |
|---|---|
| turns | 72 in 30 minutes, and the rotation was running at all 60 polls |
| heap | 13,776 B at the start, 13,868 B at the end: flat. Transient dips to about 9.3 KB, which is the web server's response buffer and the index, not the rotation |
| duty ledger | not one backwards step across 72 retunes |
| the hour | 306 s of airtime spent in 30 minutes, against a 360 s allowance |

That last row is the one to plan around: this bench is busy and the
station was mirroring onto both networks, so it was on course to spend
its whole hour and start deferring. One allowance, two networks. On a
quiet site it will not matter; on a busy one, lower
`[meshtastic]/[meshcore] broadcasts_per_hour` before blaming the radio.

**The honest recommendation** is unchanged by any of it: a site that wants
both networks reliably runs two boards, one per network, and lets XPRS
carry between them over BLE, the LAN or ESP-NOW, where XPRS's own
store-and-forward (mailboxes, XPRS.md 9.12) applies. The rotation is for
the site that has one radio and would rather reach both networks
imperfectly than one of them well.

### Using it

`cfg rotate` prints the state, `cfg rotate meshtastic,meshcore` starts it,
`cfg rotate off` stops it and stays where the radio is. On the T-Deck's
Settings panel the "LoRa mode" row steps xprs, meshtastic, meshcore, then
`both` where that mode is available, and then the rotation; `[lora] rotate`
/ `rotate_s` make it survive a restart. `rotate` refuses the word `both`,
which is section 11's business, not a ring's. `/api/status`
carries `lora.rotate` with the ring, the slice, the turns served and how
far into the current one the station is; `lora.mode` still says which
network the radio is on THIS moment.

A station that is taking turns names **both** networks in its beacon's
`serve:`, because a reader deciding where to send a message needs to know
this station reaches that network at all.

MeshCore needs PSRAM, so a board without it (the Heltec V3) cannot rotate
into MeshCore; such a mode is dropped from the ring with a line saying so,
and a ring with fewer than two usable networks is refused outright rather
than silently becoming a mode change.

## 11. One channel, both networks (`both` mode)

One radio, two networks, served at the same time instead of in turn:
`[lora] mode = both`. Like the rotation above it is a real feature and a
real compromise, and the compromise is again the important half.

**Both networks have to be on one channel already, and putting them there is
not something this firmware can do.** A receiver hears one modulation and
one sync word (section 10, and it is still true). So `both` mode does not
find Meshtastic and MeshCore where they live; it listens to one channel and
sorts out what arrives on it. Stock nodes are not on that channel until
somebody moves them.

**And the sync word is the part that bites.** Frequency, spreading factor
and bandwidth are settings on both stock firmwares. The sync word is not:
Meshtastic has 0x2B in `RadioInterface` and MeshCore passes
`RADIOLIB_SX126X_SYNC_WORD_PRIVATE` (0x12) in `CustomSX1262.h`, both
compile-time constants with no config field. So whichever value `[both]
sync` takes, the other network's nodes need a rebuilt firmware to turn up.
That is measured, not assumed: two identical radios on one channel, differing
only in the sync word, heard 0 frames of 6 from each other while 8.7 seconds
of real airtime went out, and 10 of 6 once the words matched (section 14).
`[lora] sync` exists to have asked.

**One modulation cannot suit both, either.** At SF11/250 kHz a MeshCore node
pays about four times its native airtime; at SF8/62.5 kHz a Meshtastic node
loses the link budget LongFast was chosen for. The operator decides which
network is inconvenienced. There is no setting that is good for both and the
firmware does not pretend there is one.

### The channel is configuration, and no preset exists

```ini
[both]
frequency = 869618000      ; Hz or MHz, required
sf = 8                     ; 7 to 12, required
bw_khz = 62                ; 62, 125, 250 or 500, required
sync = 12                  ; hex, required
preamble = 16              ; optional, 16 by default -- both networks use 16
```

Deliberately NOT `[lora] frequency`, `sf`, `bw_khz` and `sync`. Those are an
override for EVERY mode, which is what they are for and is also how a boot
mode's channel once followed the radio into the other two (section 3, found
on the bench 2026-09-20). A channel only one mode means is stated only for
that mode, and `[lora] frequency` is ignored here with a line saying so.

So the mode serves **one** network's stock nodes, the other network's rebuilt
ones, and XPRS stations in this same mode. That is the honest ceiling on it.

The mode is unavailable until all four are set, and it says which of the two
refusals it is: a missing channel, or a board without the memory. On a board
without PSRAM there is no row for it in `k_modes` at all
(`XPRSLORA_MODE_TABLE` in `xprslora.h`), because 216 bytes of table was enough
to cost the Heltec V3 its UI task. `cfg set
both_*` writes NVS and nothing more, so `cfg lora` reads the four keys again
before it answers: set them and ask for the mode in the next breath and it
is taken at once, with no restart, like every other channel change here. Everything
that walks the modes asks the same question (`xprslora_mode_available`), so
the T-Deck's Settings row, the sweep and the rotation's ring all skip it at
once and for the same reason.

`[lora] region` still applies, and in this mode a region row is an
**allowance rather than a channel**: there is one row, `cfg`, whose
frequency is whatever `[both] frequency` says and whose hour and e.r.p.
ceiling are EU band g3's. A station that meters against nothing transmits
without limit while believing itself compliant, so it meters against that;
whether band g3 is right where the station stands is the operator's answer,
exactly as the power ceiling already is.

### What the classifier proves, and what it throws away

`lr_class.c`, platform-free and host-tested (`test_class_host.sh`), the same
arrangement and the same reason as `lr_rotate.c`: it is arithmetic, and
arithmetic can be tested.

**It must give ONE verdict, and that is the load-bearing decision.** Both
engines relay what they cannot read, because that is what a router does on
either network, and both add a frame to their duplicate ring before they
decide anything. Hand every frame to both and you get a cross-protocol
corrupting repeater: `mt_mesh.c` would re-air a MeshCore advert as a
Meshtastic frame with a MeshCore payload byte decremented, `mc_mesh.c` would
append its own path hash into the middle of a Meshtastic payload, both
`rx_frames` counters would be fiction, and each network's dedup ring would be
permanently seeded with the other's hashes -- MeshCore's survives a reboot.

So: structure decides, and only positive evidence counts.

| | the test | what it proves |
|---|---|---|
| MeshCore is ruled out | `frame[0] >> 6` is not 0 | every current MeshCore frame has byte 0 at or under 0x3F, and a Meshtastic broadcast's byte 0 is 0xFF. This one check settles most of the traffic on a public channel |
| MeshCore structure | version 0, a payload type that exists (0x00-0x0B or 0x0F, so 0x0C to 0x0E are a rejection), transport and path and payload lengths adding up to exactly the frame, and a per-type minimum payload: an advert at least 101 bytes (key, timestamp, signature, flags), an ACK 4, a direct message or path over 3 | strong structure. This per-type table is the validation `mc_parse` does not do, and without it `mc_parse` accepts almost any byte string |
| Meshtastic structure | 17 bytes or more, `from` not 0, `hop_limit` not above `hop_start`, and either a broadcast `to` or a channel hash that exists here (LongFast's, XPRS's own 0x09, or 0 for a public-key direct message) | strong structure |
| both fit: 0 | exactly one side claims the frame as XPRS's own -- the clear XPRS channel hash on Meshtastic's side, a `RAW_CUSTOM` payload type on MeshCore's, which pins the whole first byte to 0x3C-0x3F | the side that claims it. Checked first because our OWN traffic in MeshCore wrapping can satisfy Meshtastic's structure by coincidence, and without this it would reach the end of the ladder and be dropped. When both or neither claim it, the coincidence is the thing being tested, so it decides nothing |
| both fit: 1 | a four-byte Meshtastic address that is ours, one of our virtual nodes', or one we have heard | proof, to one part in four thousand million. This is the check that saves the frames we must not lose: a public-key direct message to one of our virtual nodes can never be decrypted, so it can never prove itself any other way |
| both fit: 2 | the frame decrypts under Meshtastic's default key and decodes as a `Data` | proof. A key and a protobuf agreeing is not a guess. About fifteen AES blocks, on the bearer task, which is what rule 12 allows there |
| both fit: 3 | a one-byte MeshCore hash that is ours or a path hop we have heard | a tie-break, never a proof, which is why it is last |
| nothing matched | -- | counted (`lora.both.either`, `lora.both.neither`) and **dropped** |

Dropping costs this station one packet. Guessing costs the channel a frame
re-aired as the wrong protocol and a correct copy swallowed afterwards as a
duplicate by whichever ring was poisoned. That trade is the whole design.

**What it loses, stated rather than hidden.** A Meshtastic public-key direct
message between two OTHER nodes carries nothing we can match and nothing we
can decrypt, so roughly a quarter of them -- the ones whose `to` low byte has
its top two bits clear -- land in `either` and are dropped instead of
relayed. `lora.both.either` is the number that says how often; a rising one
means the two networks are too alike on this channel to be told apart.

### XPRS's own traffic

Understood in both wrappings, aired in one. Aired in **Meshtastic's**: 233
bytes of payload against MeshCore's 183, and a stock Meshtastic router
relays a frame on a channel hash it cannot read while a stock MeshCore
repeater does not carry a `RAW_CUSTOM` (section 9), so the Meshtastic
wrapping is the one that reaches past direct range. Both are understood on
receive because a neighbour may be in `meshcore` mode on this very channel;
airing both would double the hour for no new reader.

### One hour, now spent twice as fast

Unchanged and load-bearing: one airtime table built from the bytes the radio
was actually handed, one duty ledger, one `xb_spend`. In this mode there is
one channel, so one region's allowance covers both protocols -- simpler than
the rotation's case, where two channels in one sub-band share one allowance.

What changes is the drain. Two repeaters and two bridges spend that one hour
AT THE SAME TIME rather than in turn, and the rotation already spent 306 s
of a 360 s allowance in thirty minutes while serving each network half the
time. Expect the budget to be the binding constraint on a busy site. The
levers are the ones that already exist: `mt_repeat`, `mc_repeat`,
`mt_bcast_hr`, `mc_bcast_hr`, `lora_duty_ms`.

### PSRAM only, and the refusal

Both bridges resident plus MeshCore's worker: MeshCore's state and
Meshtastic's both in PSRAM, and a 6 KB task stack that FreeRTOS cannot put
there. The Heltec V3 has about 11.8 KB of internal heap free with ONE bridge
up and a 5.0 KB minimum-ever, so the arithmetic does not close by a wide
margin and the answer is the board's, not the heap's (section 13 and
`docs/esp32.md`): **no PSRAM, no `both`.**

The budget is taken before the radio moves and it counts BOTH blocks, so a
station that cannot afford both bridges refuses before the retune rather
than coming up on a shared channel with one bridge working and one silent.
Only MeshCore's block is claimed there, though: `xprslora_mt_start` owns
Meshtastic's and is the only thing that initialises it, so a block handed
over early would make that function's "already up" shortcut hand back a
bridge that was never built. One place budgets, the other claims. A board
asked for this mode in `config.ini` comes up in its default mode with a line
nobody can miss, and the setting is left alone so a board with room takes it
next time.

### It is not a network, so it does not take turns

`[lora] rotate` refuses the word where it is typed, and so does
`xprslora_rotate_start`: `both` is the OTHER answer to the question a
rotation answers, and a ring containing it would be taking turns with
itself. Switching into `both` stops a running rotation out loud. A sweep
skips it too -- a sweep asks which of these networks is reachable, and this
is not one of them -- but a sweep STARTED from `both` mode comes home to it
with its channel intact.

### When to use two boards instead

The honest recommendation has not changed and this mode does not change it.
A site that wants both networks reliably runs two boards, one per network,
each on its own network's real channel, and lets XPRS carry between them
over BLE, the LAN or ESP-NOW, where XPRS's own store-and-forward applies
(XPRS.md 9.12). Beyond that:

- a shared channel should carry the repeaters of **at most one** network.
  Our station repeats correctly because it classifies; a stock repeater of
  either network does not classify at all, so it will mangle the other
  network's frames and nothing in this firmware can stop it;
- the rotation is for a site that would rather reach both networks
  imperfectly, as they really are;
- `both` is for a site that controls both sides and would rather have them
  on one channel all of the time than on two channels half of the time.

## 12. Configuration

Everything here is `config.ini` and `cfg set <nvs key> <value>` over
serial. The radio settings are one section, and each bridge has its own.

| section | key | NVS | default | |
|---|---|---|---|---|
| `[lora]` | `mode` | `lora_mode` | `meshtastic` | the mode the station comes up in; every live switch writes it |
| | `region` | `lora_region` | `eu` | which of the running mode's presets (section 3) |
| | `frequency` | `lora_freq_hz` | empty | an override for EVERY mode; empty means each mode's own channel, which is what it should normally be. Refused in `both` mode, whose channel is `[both] frequency` |
| | `sf` / `bw_khz` | `lora_sf`, `lora_bw_khz` | empty | follow neighbours onto a modulation this firmware has no preset for (7 to 12; 62, 125, 250, 500). Not consulted in `both` mode, which takes its modulation from `[both]` |
| | `profile` | `lora_profile` | empty | `far` is SF9, `xprs` mode only |
| | `duty_ms` / `reserve_ms` / `pace_ms` | `lora_duty_ms`, `lora_resv_ms`, `lora_pace_ms` | the region's | the hourly budget, the slice held back for sos and warnings, and the gap between our own frames |
| | `local` | `lora_local` | `no` | LoRa counts as a local bearer (9.11.1) |
| | `sync` | `lora_sync` | empty | the sync word, hex; empty is the mode's own (`xprs` and `meshcore` 0x12, `meshtastic` 0x2B). It exists to ask one question nothing else can, because neither foreign firmware exposes its own: does this chip reject a frame whose sync word is not the one it was set to (section 14) |
| | `survey_s` | `lora_survey_s` | 60 | how long the listen-only survey sits on each mode |
| | `detect_s` | `lora_detect_s` | empty | one figure for auto-detect (5 to 300); empty means each network's own wait |
| `[both]` | `frequency` | `both_freq_hz` | **required** | the one channel the two networks were put on, Hz or MHz. Nothing defaults: there is no shared channel to default to |
| | `sf` | `both_sf` | **required** | 7 to 12 |
| | `bw_khz` | `both_bw_khz` | **required** | 62, 125, 250 or 500 |
| | `sync` | `both_sync` | **required**, and it has no default on purpose | hex. Whichever value this takes, the other network's nodes need a rebuilt firmware to meet it (section 11), so an operator who has not thought about it is stopped rather than handed one network's value by accident |
| | `preamble` | `both_preamble` | 16 | both networks use 16, so this is almost never set |
| `[meshtastic]` | `repeat` | `mt_repeat` | `yes` | relay Meshtastic frames |
| | `bridge` | `mt_bridge` | `yes` | translate both ways; adds `meshtastic` to `serve:` |
| | `broadcasts_per_hour` | `mt_bcast_hr` | 12 | XPRS broadcasts mirrored onto LongFast |
| | `nodeinfo_min` | `mt_ni_min` | 180 | how often our nodes re-announce |
| `[meshcore]` | `repeat` | `mc_repeat` | `yes` | relay MeshCore frames |
| | `bridge` | `mc_bridge` | `yes` | translate both ways; adds `meshcore` to `serve:` |
| | `broadcasts_per_hour` | `mc_bcast_hr` | 12 | XPRS broadcasts mirrored onto the public channel |
| | `advert_min` | `mc_advert_min` | 180 | how often each virtual node re-advertises |

`[lora] rotate` and `mode = both` are the two answers to one question, so
they are mutually exclusive and the station says which it took: `rotate`
refuses the word `both` where it is typed, and switching into `both` stops a
running rotation.

The console words are `cfg lora <mode>`, `cfg freq <MHz|Hz|preset>`,
`cfg region <name>`, `cfg survey [s]` and `cfg detect [s]`; `docs/API.md`
has them with their output, and the T-Deck's Settings panel has a row for
each of the four. `cfg lora` with no argument also prints the `[both]`
channel, or says it is not set.

## 13. Memory and stack

| | T-Deck (PSRAM) | Heltec V3 (no PSRAM) |
|---|---|---|
| bridge state | 9.2 KB, in PSRAM | 6.7 KB internal (smaller tables: `MT_SMALL`) |
| free internal heap, steady | 21 to 25 KB (unchanged) | 11.8 KB (14.5 without the bridge) |
| minimum ever | 15.3 KB | 5.0 KB after 90 s, still falling slowly: watch it |
| bearer task stack never used | | 2.35 KB of 7 KB, X25519 included |

The Heltec did not fit as it was: with the bridge on and LVGL's pool at 16 KB,
free heap was 7.9 KB and BLE advertising failed (`BLE_INIT: Malloc failed`).
The pool was measured at 8.2 KB used and cut to 12 KB, which is what the
board's `sdkconfig.defaults` now says. The defaults had said 6 while the
generated `sdkconfig.heltec_v3` said 16, and the generated file is what built.

The curve work (X25519 for a DM or a NodeInfo, about 1.3 KB deep) runs only
on the bearer task's tick, never on whichever task heard the packet.

**`both` mode holds both bridges at once**, which is where the arithmetic
stops closing on a board without PSRAM: MeshCore's state (10,064 bytes) and
Meshtastic's (9.2 KB) both want PSRAM, and the `mcwork` task's 6,144-byte
stack cannot go there at all because FreeRTOS stacks are internal. Against
the Heltec V3's 11.8 KB of free internal heap with ONE bridge running, that
is not a reshuffle, it is a feature given up -- the fourth time this page's
rule has given that answer (`docs/esp32.md`, "The arithmetic that did not
close"). The classifier adds 255 bytes of `.bss` for the one buffer its
decrypt proof writes into, and nothing else. The measured figures for a
T-Deck in `both` mode go in section 14 when the soak has run.

## 14. Measured on the bench

### Meshtastic, 2026-09-19

T-Deck X3DCK0 and Heltec V3 X3H3MZ on this firmware, a second T-Deck on stock
Meshtastic 2.7.26 (EU_868, LongFast, node `0x0C39F654`) as the witness.

| | result |
|---|---|
| witness lists both stations | `X3DCK0`, `X3H3MZ`, hardware `PRIVATE_HW`, key present |
| witness LongFast text into XPRS | both bridges composed `t:message f:MT0C39F654 …`, `scope:local` |
| witness DM to X3DCK0 | translated to `d:X3DCK0`, acked within 2 s |
| witness DM to X3H3MZ | acked by its node; the T-Deck translated it too |
| XPRS DM `d:MT0C39F654` | shown on the witness, acked; signed receipt back in XPRS |
| XPRS broadcast | shown on the witness from `X3DCK0` |
| witness reply to it | `r:` naming the XPRS original |
| DM before the key was known | `PKI_UNKNOWN_PUBKEY`, NodeInfo came back, the DM held for the key went |
| key after a restart | remembered; the next DM opened at once |
| our XPRS frames through the witness | relayed intact, hop 3 to 2 |
| desktop app DM `d:MT0C39F654` over LAN | first try: sealed under a key the witness had not heard, dropped on every retry (same id); after the fix, witness NAK 35, NodeInfo to it, DM again under a new id, opened, gateway receipt released the desktop's held copy |
| desktop app, sealed DM to MT | refused, `cannot seal: foreignNetwork` |
| desktop app sees the witness | `kind: foreign`, `network: meshtastic` |

The Meshtastic phone app (Android 2.8.1 on the OUKITEL C61), connected over
Bluetooth to the witness, 2026-09-19:

| | result |
|---|---|
| node list | `X3DCK0`, `X3H3MZ`, `X16JK8` with keys, `PRIVATE_HW`; offline with last heard "Unknown" until one of them sends a NodeInfo or a text |
| DM to X16JK8 (the desktop) | first try: acked, never reached XPRS (the `via:` refusal above); after a reflash, flooded past both bridges (no callsign table); fixed: "Delivered to recipient" and shown on the desktop, also after a restart of both bridges |
| reply from X16JK8 | shown in the app's conversation; the T-Deck's gateway receipt released the desktop's held copy |
| post on LongFast | reached XPRS once, `scope:local` (the app leaves OK-to-MQTT off) |
| XPRS broadcast | shown in the app's LongFast channel, from `X16JK8` |
| our stations as the app's radio | not possible: they do not offer Meshtastic's phone API over Bluetooth, so the app lists only real Meshtastic nodes |
| live mode switch, T-Deck | `cfg lora xprs`: retuned to 869.5 MHz SF7 sync 0x12 with the uptime unbroken, XPRS heard from the P1-Pro and the Heltec; no Meshtastic frame aired in 40 s afterwards, and `lora.mt` gone from the status; back to `meshtastic` the same way |
| survey, T-Deck, 25 s a mode | `xprs` 7 frames naming X3DCK0, X1UDP4, X1ARKL; `meshtastic` 0 frames in that window; back in the mode it started in, and the whole sweep in `/api/status` under `lora.survey` |
| desktop chat wapp DM to an unheard node (`MT0BADCAFE`) | left by the T-Deck: `dm_not_here` counted, `key_asks` 0, nothing aired |
| desktop chat wapp DM to the witness | delivered (`text_out` 1, `dm_acked` 1), shown in the phone app; the Heltec's receipt released the desktop's held copy |

LoRa modes, the same day:

| | result |
|---|---|
| default boot | `meshtastic`, 869.525 MHz, bridge running (regression) |
| `cfg set lora_mode xprs` on T-Deck and Heltec, restart | `up in xprs mode: 869500000 Hz SF7/125k, sync 0x12`; the two exchange XPRS on LoRa, beacons included; `lora.mode` `xprs`, no `lora.mt`; Heltec 18.9 KB free (11.8 in `meshtastic`) |
| the P1-Pro | heard again: packets relayed `via:...,X3S7S8` at -65 dBm |
| T-Deck Settings row "LoRa mode" | OK switched the saved mode to `meshtastic`, shown as "XPRS+Meshtastic (restart)"; after the restart it runs `meshtastic` |
| owner's `cmd:set lora:` | host-tested (setup rules, the Firmwares wapp harness); not run on the bench: neither board is owned by a profile on the bench's phone or desktop |

### MeshCore, 2026-09-20

Everything above was written from MeshCore's published format and tested
against a simulation of it. Then it met a real node, and the bench
corrected five things that no host test could have caught. The setup: a
Heltec V3 running the published MeshCore build (repeater v1.17.1, later the
companion build driven from a desktop over USB) and a T-Deck running this
firmware in `meshcore` mode.

1. **The channel is not LongFast with another sync word.** A stock node
   answers `get radio` with `869.6179809,62.5,8,5`: 62.5 kHz and SF8, not
   250 kHz and SF11. Its source sets the preamble to 16 and the sync word
   to `RADIOLIB_SX126X_SYNC_WORD_PRIVATE` (0x12). Our mode row had three of
   the four wrong, and a station on it would have heard nothing at all.
   Worse, the bearer's spreading-factor lookup was a chain of comparisons
   that read anything unfamiliar as SF11, so the duty ledger would have
   charged five times the real airtime.
2. **An advert's name is not where it looks.** The app data is a flags byte
   and then OPTIONAL BLOCKS in a fixed order -- location, two feature words
   -- and only then the name. Reading the name straight after the flags
   gives an empty name, which is what a real repeater's advert produced
   until it was parsed properly (`mc.h`, and MeshCore's
   AdvertDataHelpers.cpp).
3. **The acknowledgement is hashed over the message's first five bytes.**
   `sha256(timestamp(4) || type-and-attempt(1) || text || sender key)`, and
   we were leaving out the fifth byte. The checksum then matches nothing,
   the sender retries three times and tells its user that nobody answered.
4. **An ack can arrive inside a PATH.** A client with no route back answers
   a first direct message with a PATH return (type 0x08) that carries the
   acknowledgement in its `extra` field, not with an ACK packet. A bridge
   that waits for type 0x03 never hears it.
5. **The advert has to go before the message.** A MeshCore client can only
   open a message from a contact it knows, and it learns the key from an
   advert: a direct message that overtakes its own advert is unreadable.
   This is the same lesson Meshtastic taught with NodeInfo, and this
   firmware had to learn it twice (`MC_AFTER_ADVERT_MS`).

**And one that only a second radio could answer: a stock MeshCore repeater
does NOT relay our `RAW_CUSTOM`.** Its own log shows our frames arriving
cleanly (`RX, len=186 (type=15, route=F, payload_len=184) SNR=12`), and it
re-airs our adverts and our channel messages, but it never re-airs type 15.
So XPRS over MeshCore's channel reaches stations in direct radio range and
no further, exactly the fallback the plan named. What crosses the whole
MeshCore mesh is what is translated: channel messages and direct messages.

**What was proved working, both ways, against that node:**

| | |
|---|---|
| our station in a client's contact list | `adv_name: X3HW9U`, type chat, signature verified by their code |
| XPRS broadcast to MeshCore's public channel | heard and relayed by the repeater |
| MeshCore channel message to XPRS | `t:message f:MC7A5F15D8 scope:local zmid:… via:X3HW9U m:…`, carried on by other XPRS stations |
| MeshCore direct message to an XPRS callsign | opened (ECDH + MAC), delivered, acknowledged |
| XPRS direct message to a MeshCore node | sealed, acked through their PATH return, gateway receipt signed and delivered |

**A phone app cannot see our stations directly.** The official MeshCore
Android app is a companion to a board flashed with MeshCore's companion
firmware and talks to it over Bluetooth, USB or TCP; it has no radio of its
own. Scanned next to a live XPRS station it finds nothing, exactly as the
Meshtastic app does. Our stations reach its user through the node it is
paired with, which is where all of the above was measured.

**The one thing the bench has already answered:** does a stock MeshCore
repeater flood a `RAW_CUSTOM` packet it cannot read? No, it does not (the
section above). XPRS on that channel therefore crosses between stations in
range of each other, and the mode loses MeshCore's relays for XPRS traffic
while keeping them for everything the bridge translates.

### `both` mode on the air, 2026-10-03

Two T-Decks (X3DCK0 and X3HW9U, both PSRAM) and a Heltec V3 (X333SM, none).
There is no stock node of either network at this bench, so each protocol's
traffic came from a station of ours put on the shared channel: the Meshtastic
frames are genuine Meshtastic frames and the MeshCore ones genuine MeshCore,
aired by a real SX1262, which is what the classifier has to tell apart.

**The sync word: a mismatch is rejected, and that is the answer the mode's
reach depends on.** Two identical radios on an IDENTICAL channel -- 869.618
MHz, SF8, 62.5 kHz -- differing in nothing but the sync word:

| A's sync | B's sync | B aired | A heard |
|---|---|---|---|
| 0x2B | 0x12 | 8,665 ms | **0 of 6** |
| 0x12 | 0x12 | 5,885 ms | 10 frames from 6 probes |

So the comparison is strict, and `[lora] sync` exists to have asked. Meshtastic
hardcodes 0x2B and MeshCore 0x12, neither exposes it, and therefore **one
channel cannot carry both networks' stock nodes.** Whichever value `[both]
channel` takes, the other network's nodes need a rebuilt firmware. The mode
serves one network's stock nodes, the other's rebuilt ones, and XPRS stations
in the same mode. Nothing in this firmware can change that.

**Classification, one channel, both protocols.** X3DCK0 in `both` mode on
869.618/SF8/62.5/0x12; X3HW9U put on the same channel first as a MeshCore
station (native there) and then as a Meshtastic one (`lora_sync 12`). Over
twenty minutes:

| verdict | count | what followed |
|---|---|---|
| `mc` | 2 | to the MeshCore engine; `mc.relayed` 2 -- its repeater re-aired both |
| `mt` | 1 | to the Meshtastic engine; `mt.relayed` 1 |
| `xprs_mc` | 2 | XPRS wires, unwrapped and delivered |
| `xprs_mt` | 18 | the same in Meshtastic framing |
| `either` | **0** | -- |
| `neither` | **0** | -- |

Not one frame was ambiguous and not one was dropped, and neither engine's
counter ever moved for the other protocol's traffic. That is the claim the
mode makes, measured.

**The refusals.** The Heltec V3 answers `cfg lora both` with "this board has
no PSRAM, and two bridges plus a 6 KB stack cannot be had without it" and does
not move the radio; `both` is not in its `k_modes` at all
(`XPRSLORA_MODE_TABLE`). A T-Deck with no `[both] channel` set answers "both
needs a channel first". `cfg rotate meshtastic,both` is refused where it is
typed, `cfg rotate meshtastic,meshcore` still works from `both` mode, and
switching into `both` stops a running rotation with a line saying why.
`spent_ms` went 33,594 to 34,468 across that switch: the hour does not restart
because the modem moved.

**LDRO, and why the old condition was a bug.** At SF11/62.5 kHz a symbol is
32.77 ms, so low data rate optimize must be on; the old condition
(`BW125 && (SF11 || SF12)`) left it off. Both boards on that channel with the
same sync word, one running the old code and one the new:

| A (new, LDRO on) | B | B aired | A heard |
|---|---|---|---|
| SF11/62.5 | old code, LDRO off | 32,473 ms | **0 of 3** |
| SF11/62.5 | new code, LDRO on | 13,536 ms | 2 of 3 |

A dead link, silently, on a modulation `both` mode invites an operator to
choose. Found by reading the datasheet rule against the code, confirmed here.

**What the hour costs.** Those SF11/62.5 kHz probes are over four seconds
each, and twenty minutes of them spent 280,829 ms of the 360,000 ms allowance.
On this mode's intended channel (SF8/62.5) a frame is a fifth of that, but the
point stands: two bridges on one hour drain it, and `broadcasts_per_hour` is
the lever.

**On a fully loaded T-Deck, with nothing turned off (2026-10-03).** The first
version of this mode could only be run with `ble_on = no`, because
`lr_claim_for` wanted 22,528 bytes of free internal heap and a loaded T-Deck
had about 22.0 -- and refused plain `meshcore` mode for the same reason, which
is how the margin was found. Two fixes closed it, neither of them a reserve
being shaved:

- the worker's 6 KB stack is claimed at start, from a whole heap, instead of
  at the moment a mode is entered from a fragmented one, so entering costs
  nothing (`docs/esp32.md`, "Create the big task stacks first");
- the board got back about 9.6 KB of internal DRAM that
  `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` had quietly spent on six extra WiFi static
  RX buffers (`docs/esp32.md`, "A directive asked for is not a directive
  taken").

With BLE up and nothing disabled: `cfg lora meshcore` succeeds, `cfg lora
both` succeeds, min-ever is **14,872** against the 4,000 floor, and the
classifier's figures over the run were `mt` 7, `mc` 7, `xprs_mt` 4,
`xprs_mc` 8, **`either` 0 and `neither` 0**. Both repeaters relayed what they
were handed and neither engine's counter moved for the other protocol's
traffic.

### A phone's message on both networks, and 20 minutes of it (2026-10-03)

The whole chain, end to end, with a phone that has no radio at all.

**The topology.** Two T-Decks in `both` mode and a Heltec V3 as a Meshtastic
station, all three on one channel (869.618 MHz, SF8, 62.5 kHz, sync 0x12).
The Heltec cannot run `both` -- no PSRAM -- but it can be a single-network
station on the shared channel, which is what a site with one such board
would do.

**The send.** A phone (X1WATT) broadcast on the local channel through its own
send path, and said what it had used: `ble5 sent, lan sent, reticulum sent,
lora inactive`. A phone has no LoRa; the stations are what put the words on
the air.

**What each station did with it**, measured against an idle baseline of
exactly zero on every counter:

| | `mt.text_out` | `mc.text_out` | `mt.relayed` |
|---|---|---|---|
| X3DCK0 | +1 | +1 | +1 |
| X3HW9U | +1 | +1 | 0 |

One XPRS message, translated onto **both** foreign networks by each station,
and relayed on the air exactly once. And the range extension, which is the
point of the exercise -- the Heltec, which the phone cannot reach by any
other means, logged it off the radio:

```
RX 170 bytes at -39 dBm SNR 12: t:message f:X1WATT ts:2026-10-03_19:43:13
```

**EACH GATEWAY TRANSLATES INDEPENDENTLY, AND THAT IS THE DECISION.** Two
stations in earshot put the same broadcast on each network, once each, so a
foreign network sees one copy per gateway rather than one copy. That is not
the duplicate-suppression rule failing: the suppression that stops a second
gateway airing something (`origin != MT_XPRS_OWN`, `mt_mesh_on_xprs`) is for
DIRECT messages, where a reply that reached every bridge on the internet must
only be aired by the bridge that can hear the recipient. A broadcast is
different -- a reader on either network may be in range of only one of the
gateways -- so every gateway announces it, each as its own packet with its own
`via:` (rule 8), and the rate is held by `broadcasts_per_hour` rather than by
silence. Do not "fix" this into one gateway speaking; it was considered and
kept on 2026-10-03.

What IS deduplicated, and was measured here, is the REPEAT: several stations
hear one frame and only one re-airs it. `mt.relayed` moved once, `relay_skipped`
stayed at zero, and `mc.rx_dupes` climbed as each station recognised the
other's frame rather than carrying it again.

**Twenty minutes unattended**, 40 samples per station, with the phone's
traffic and the fleet's own crossing the channel throughout:

| | X3DCK0 | X3HW9U | X333SM |
|---|---|---|---|
| min-ever internal | 15,836 to 15,364 | 15,440 to 14,968 | 27,704 free, 23,480 largest |
| reboots | 0 | 0 | 0 |
| `both.either` / `both.neither` | **0 in all 40 samples** | **0 in all 40 samples** | n/a |

The classifier never once produced an ambiguous or unplaceable verdict over
the whole run. The number to plan around is the hour: X3DCK0 spent 82 s of
airtime in those 20 minutes, which extrapolates to about 68% of its 360 s
allowance. Two bridges on one channel, both translating every broadcast, is
what that costs, and `broadcasts_per_hour` is the lever.

### FIXED, without the cause ever being named: the watchdog panic in `both` mode (2026-10-03, closed 10-04)

**The reboots have stopped, and that is not the same as knowing why they
happened.** Both T-Decks used to reboot every fifteen to thirty minutes in
`both` mode with `ESP_RST_TASK_WDT`, while a Heltec V3 on the same channel did
not. With the watchdog fed per flush slice and the screenshot capture given
one deadline for the whole frame, both boards ran an hour unattended:

| | X3DCK0 | X3HW9U |
|---|---|---|
| uptime at the end | 3,966 s | 3,937 s |
| reboots | 0 | 0 |
| `starved` reported | none, so it never fired | none |
| heap floor | 15,360 to 13,200 | 13,300 flat |
| packets classified | 507 | 475 |
| `either` / `neither` | 0 / 0 | 0 / 0 |

Then twenty-one more minutes on the shipped image, also clean. Twenty minutes
proves nothing here, which is the mistake that let this be reported as stable
once before; an hour is the bar.

**`starved` is still empty, and that is the honest state of it.** The capture
below works, rehearsed and proven, but the panic it was built to explain has
not happened since the feeds went in. So the fix is a bound on the damage a
slow pass can do, not a repair of whatever made a pass slow. If a board in
this mode ever reboots again, `zw:` on the beacon and `starved` in `/api/diag`
will name the task, and the branches at the end of this section say what to do
with each answer.

**The first two diagnoses here were wrong, and the way they were wrong is
worth more than either of them.**

The first blamed UI starvation on the bridge mutex. It is disproved: the `ui`
task does not take `s_mt_mutex` on any repaint path. Every `xprslora_*` call
`ui_render` makes is lock-free, and the two that do lock
(`xprslora_mt_stats`, `xprslora_mc_stats`) are reached only from `idx_task`
and the httpd task.

The second blamed IDF's USB-serial-JTAG console, which really does busy-wait
up to 50 ms per character when no host drains the CDC
(`vfs_usb_serial_jtag.c`, `usb_serial_jtag_tx_char`). Also disproved as the
cause here: `ui` barely logs, the 22 lines a second come from tasks `ui`
outranks, and the Heltec's immunity has a simpler explanation.

**The Heltec is immune because it builds no LVGL at all.** It uses
`xprs_ui_mini`, and `lvgl` appears zero times in its configuration. Nothing
about LoRa explains the difference; the display path does.

**What misled both attempts** is in `docs/esp32.md` now, because it is a trap
anyone reading a crash record will fall into: **`exc_task` names the task
that was RUNNING when the panic fired, not the task that starved the
watchdog.** Both watched tasks are pinned to core 1 and `ui` is the
highest-priority task there, so "ui" is the expected answer for any core-1
trigger, including one caused by `idx`. The watchdog's ISR does know the right
answer and prints it with `ESP_EARLY_LOGE`, which bypasses the log hook and
therefore survives nothing: not `/api/log`, not the RTC ring, not `zc:`.

**So the firmware now records it.** `esp_task_wdt_isr_user_handler` is a weak
symbol and `esp_task_wdt_print_triggered_tasks()` is public; `xprs_diag` uses
both to write the starving task's name into RTC memory that outlives the
reboot, and reports it as `zw:` on the beacon and `starved` in `/api/diag`,
separately from the core dump's `crash.task`. Rehearsed on the bench with a
deliberate spin before being trusted, and that rehearsal corrected how both
fields read: `crash.task` named the task that was spinning and `starved`
named a lower-priority one it had starved of CPU, so the runner is the suspect
and the starved task is the evidence (docs/esp32.md, "The crash record names
the task that was running"). Three tasks subscribe here, all on core 1: `ui`
at priority 4, `idx` at 3, `script` at 2.

Note what that does to the four earlier records, which showed `crash.task` as
`ui` three times and `idx` once: on the rehearsal's reading those name the
task that would not yield, which puts the UI and indexer paths in the frame
and leaves the classifier and the bridges out of it. That is a direction, not
a cause, and `starved` is what will settle it. The direction did hold: what
stopped the reboots was a feed in the UI flush path, and nothing in the
classifier or either bridge was touched to achieve it.

**Also established, and relevant whatever it names:**

- the watchdog does not watch the idle tasks. `xprs_app.c` reconfigures it at
  boot with `idle_core_mask = 0`, so the watched set is `ui` and `idx` only,
  at 90 s with panic. A trigger therefore means one iteration of one of those
  two tasks took over ninety seconds;
- `ui` feeds the watchdog once, at the bottom of its loop, after `ui_render()`
  and `xui_update()`. Everything in a pass is one unfed window;
- the display and the radio share SPI2, and both use polling transfers.
  `spi_device_polling_end`'s wait is a bare CPU spin whose timeout can never
  fire at `portMAX_DELAY`, so a panel that stops answering hangs `ui` for
  ever. One of the four crash PCs was inside that loop. It is **still not
  bounded**, deliberately: giving it a real ceiling cost X3HW9U its radio for
  383 s (0 frames received against the control's 42), because the timeout path
  keeps the shared bus lock for ever and there is no cancel call. The trap is
  written up in `docs/esp32.md`. What the flush does instead is feed the
  watchdog per slice, which `common/xprs_lvgl/lvgl_port.c` already did for the
  e-paper board and this path did not;
- `xui_capture` waited two seconds PER ROW with nothing bounding the frame, so
  an HTTP peer that walked away mid-screenshot could hold the UI task for
  eight minutes. One deadline for the whole capture now.

`both` mode is usable on a station now, on the hour of evidence above. What is
not closed is the mechanism: something was taking a UI pass over ninety
seconds and the feeds mean it no longer has to finish to keep the board alive.
Treat a reboot in this mode as a live lead rather than a known quantity, and
read `starved` first.

## 15. Lessons learned

Each of these cost at least one wrong turn on 2026-09-19. Read them before
changing the bridge.

- **Both engines mutate before they validate, so a classifier gets one
  verdict and only on positive evidence.** `mt_mesh_on_frame` counts the
  frame, adds it to a clockless dedup ring and cancels a pending relay
  before it has any idea whether it can read it; `mc_mesh_on_frame` does the
  same on a ring that survives a reboot. "Offer it to both and let the
  winner validate" therefore is not a cheap shortcut, it is a
  cross-protocol corrupting repeater plus two poisoned dedup rings. A frame
  that cannot be placed is dropped, and the drop is counted so the cost is
  visible.
- **A sync word is a compile-time constant in both foreign firmwares**, so
  "one channel" is work on their nodes and never a setting of ours. Anything
  that promises to serve two networks has to say that in its own start-up
  line, not only in a document.
- **A condition that is right by luck is still wrong.** Low data rate
  optimize was enabled for `BW125 && (SF11 || SF12)`, which gives the right
  answer for every preset this firmware carries and the wrong one for three
  modulations an operator can now pick (SF10 and SF11 at 62.5 kHz, SF12 at
  250 kHz). The rule is symbol duration at or over 16.38 ms, it always was.
  Measured on 2026-10-03: old code against new on SF11/62.5, same sync word,
  **0 frames of 3** while 32 seconds of airtime went out. A dead link, and the
  antenna would have got the blame.
- **"Free heap went up" hid a crash.** The first version of `both` mode cost
  the Heltec V3 920 bytes of static RAM, its UI task fell back from 8 KB to 6
  and the board panicked in a loop -- while REPORTING MORE FREE HEAP than the
  build that worked, because the task it failed to create was 2 KB. This page's
  own rule is the one that catches it: judge by min-ever and by whether every
  subsystem started, never by the free number (`docs/esp32.md`).
- **A cached config key costs 88 bytes whether it is set or not.** Five keys
  for one channel cost 440 bytes of internal DRAM to hold values like "8" and
  "62", which was enough to cost the Heltec its UI task and a T-Deck its
  MeshCore worker. They are one key now, `[both] channel =
  <freq>,<bw_khz>,<sf>,<sync>`, which is also how MeshCore's own nodes state a
  channel. A channel is one thing.
- **The preprocessor cannot see an enumerator.** `#if XPRSLORA_MODE_TABLE >
  XPRSLORA_MODE_BOTH` reads as `0 > 0`, because an unknown identifier in an
  `#if` is 0 -- so the table row was silently dropped on EVERY board, and a
  T-Deck with 8 MB of PSRAM answered "both cannot run on this board". Literals
  and a `_Static_assert` now, and the bench found it in one line.
- **A log line that prints the mode's preset instead of the radio's setting
  will cost somebody an afternoon.** The old boot line read `SF11/250k` for a
  radio actually on 62.5 kHz, because it printed `s_def->bw_hz` rather than
  what was handed to the modem. It nearly invalidated the LDRO measurement
  above. Both log lines and the airtime table are built from the config struct
  the chip was given now.

- **"Delivered" on Meshtastic proves only that a bridge acked.** The bridge
  acks at once and takes custody; whether the words reached XPRS is a
  separate question. The phone app showed "Delivered to recipient" for a
  message that the relay path had thrown away. Check the XPRS side
  (the recipient's history, or `mesh` lines in the station log) every time.
- **A rule written for relays catches your own compositions.** The relay
  path refuses a wire whose `via:` names this station, correctly for a
  relay, and silently for a translation, whose `via:` names its gateway by
  design. Anything a station composes leaves through the send path.
- **Meshtastic records an id as seen before it knows it can decrypt it.** A
  DM that overtook its key was dropped on every retry. The fix was a new id,
  not a better retry; and a NodeInfo id is deterministic, so a node that
  heard it recently drops it too.
- **Whatever lives only in RAM is gone after a reflash, and the other network
  still remembers.** Meshtastic keeps every node in its node list; a bridge
  that forgot the callsigns behind its virtual nodes flooded DMs to them past
  itself. Persist what the other side relies on, and only that.
- **Receive paths park; they do not send.** Sending from inside the bridge's
  mutex made every other radio task wait on a Reticulum socket. This is
  `docs/esp32.md`'s rule, and it was broken by moving a call, not by adding
  one.
- **A test that passes without the fix proves nothing.** The first ordering
  test (NodeInfo before a DM) passed with the delay turned off. Run each new
  host test once against the unfixed code; scenarios 18 to 20 were.
- **Test through the core, not around it.** The app's `/api/xprs/send`
  composes and publishes by itself; a result from it says nothing about the
  path a person uses. Drive the chat wapp (`POST /api/wapp/cmd`,
  `rooms_send`) so the packet goes through `hal_xprs_message` and
  `XprsSend`.
- **A duplicate check belongs at the door.** The first `zmid:` check sat in
  the wapp delivery and in the courier, two places that each saw part of the
  traffic. It is in `PacketGateway` now, keyed with the part number because
  a split translation's parts all carry the same `zmid:`.
- **Driving a phone blind is dangerous.** A tap script that lost the
  Meshtastic app tapped "Send" in a wallet app on the same phone. Check the
  foreground package before every tap, and read a typed field back before
  sending: the keyboard autocorrects (`queued` became `Zurück`).
- **Moving the fleet to one network cut off a station on the other.** The
  P1-Pro, still on XPRS's own channel, went deaf to every ESP32 the day they
  moved to LongFast. A station's LoRa network is now its setting (`lora_mode`),
  and one set to `xprs` heard the P1-Pro again at once.
- **A table sized for twelve rows does not warn at thirteen.** Adding the
  LoRa mode row made the Settings panel 13 rows against `XUI_TAB_ROWS` 12;
  the constant had to grow with it (xprs_ui.h says so now).
- **A serial console that resets the board eats the first keys.** Opening
  the T-Deck's USB port reset it, and keys sent in the first seconds went to
  a board still booting. Wait out the boot before sending keys, and read the
  config back.
- **A live switch is not a smaller restart.** Everything derived from the
  channel has to move with it: the modem, the airtime table, the duty
  ledger, the pace, and which bridge may speak. The first version retuned
  the radio and left the Meshtastic bridge transmitting on a channel that
  was no longer there.
- **Two programs on one serial port put the T-Deck in the bootloader.** A
  capture and a console opened at once toggled DTR and RTS between them and
  the board came up in `waiting for download`. One connection at a time, and
  send the command down the same one that is listening.
- **Which task the arithmetic runs on is a design decision, and it is made
  with a measurement.** MeshCore signs its adverts, so reading one is an
  Ed25519 verification: 3.9 KB of stack, measured with `-fstack-usage` on
  the target compiler, against about two kilobytes spare on the bearer
  task. Written the obvious way (verify where the frame arrives, derive a
  callsign's key where the packet arrives) this firmware would have had a
  reboot loop on the first advert and another on the first XPRS packet
  heard over Bluetooth, since `mc_mesh_on_xprs` runs on whatever task heard
  it. The bridge is split instead: the receive path parks, `mc_mesh_work`
  on its own 6 KB task does every signature and key exchange, and
  `mc_mesh_tick` only airs. Meshtastic's X25519 is 1.4 KB, which is why
  that bridge never needed one, and measuring is what tells the two apart
  (docs/esp32.md, "Task stacks are heap").
- **Two locks are an order, and freeing state is where it gets reversed.**
  Every path takes the bridge's lock first and the radio lock inside it
  (the bridge ticks, then airs). Releasing MeshCore's block from inside the
  retune, which runs under the radio lock, would have taken them the other
  way round, and a bridge airing at that moment would have sat waiting for
  a task that was waiting for it. The release now happens after the radio
  lock is dropped, the free is inside the bridge's lock, and every reader
  tests the pointer INSIDE that lock rather than before it, so a task
  waiting on the lock finds NULL and not freed memory.
- **A cipher that pads can turn a long message into no message at all.**
  MeshCore's AES-ECB pads to whole 16-byte blocks, so 175 bytes of text
  fits the payload before padding and not after: the build returned 0 and
  the direct message sat in the queue until its 24-hour park, silently.
  The limit is written down once (`MC_TEXT_MAX`, 171 bytes, with the
  arithmetic in mc.h), enforced in the builders so a long message is
  shortened rather than lost, and the longest message there is now has a
  test.
- **A free-heap reading early in the boot does not answer "will this
  fit".** MeshCore's mode claims its state before the screen, the index
  and the web server take theirs, so it saw 48 KB free on a board that had
  eleven to give and reboot-looped at 1,920 bytes. The page that says
  "whoever starts last gets the fragments" had already written this down,
  and the answer it prescribes is the one that worked: put the budget on
  paper, subtract, and when it does not close decide what the board is
  for. This board does not run MeshCore.
- **A published format is a starting point, not the answer.** Five things
  about MeshCore were wrong in code that passed every host test, because
  the tests checked this firmware against itself: the channel's modulation,
  where an advert's name sits, what the acknowledgement is hashed over,
  that an ack can arrive inside a PATH packet, and that an advert has to
  precede the first message. All five took one afternoon with a real node
  on the bench, and none of them could have been found without one. Write
  the host tests, then go and measure.
- **An hourly allowance is spent by duplicates unless it is told not to
  be.** The same XPRS packet arrives over and over (its own echo on the
  LAN, a digipeat, a replay). The bridge charged the broadcast cap for
  every copy, so a station stopped mirroring after two messages and the
  counter that would have said so was not in the API. Dedup by the
  packet's own identifier BEFORE the allowance, and put every counter on
  the status page: the one you leave out is the one you need.
- **A config key that does not exist reads as empty, and nothing
  complains.** Both bridges were started with `xcfg_get("nick", "")`, while
  the station's name lives under `name`: every NodeInfo since the bridge
  shipped carried the bare callsign instead of "roof X3DCK0", and no test
  caught it because the tests pass their own nick in. Found while wiring the
  same call for MeshCore (2026-09-20). A key must exist in
  `xprs_config.c`'s tables, and a default that is also the failure mode
  hides the mistake.
- **A radio that is not advertising is not a range problem.** The witness
  had Bluetooth off in its config; the phone app saw nothing until
  `bluetooth.enabled` was set.

- **A worker that stops and never restarts looks exactly like one that
  is running.** Leaving `meshcore` mode stood the `mcwork` task down, and
  coming back did not start it again: `xprslora_mc_start` returned early
  on "the bridge is already up". The bridge went on airing its queue with
  nothing behind it -- no signature verified, no advert scheduled, no DM
  retried, nothing written to NVS -- and every counter on the status page
  looked healthy. Found while writing the rotation, which would have hit
  it on its second lap; it had been there since the day the mode was
  written. When a subsystem is stopped and restarted by two different
  paths, the restart needs a test of its own: here, that an advert (type
  0x04, which only the worker can sign) goes out after mode, other mode,
  mode again.
- **A ledger reset on a setting change is a compliance bug waiting for a
  faster caller.** `xb_set_duty()` memsets the duty ledger, and every mode
  change called it. With a mode change being an operator's rare act that
  was merely wrong; with a rotation changing mode every 25 s it would have
  reset the hour 40 times an hour and transmitted without any limit at
  all, while reporting itself compliant. The fix is `xb_set_duty_keep`,
  which changes the limits and keeps `bucket[]`, `spent_ms` and `head_ms`:
  the regulator's hour does not restart because our modem changed channel.
  Anything that reads like "reconfigure" deserves the question "and what
  does it quietly forget?".
- **A per-switch NVS write is invisible until the switch becomes
  automatic.** `lora_apply_mode` wrote `lora_mode` on every change, which
  is right for an operator's choice and would have been 40 flash writes an
  hour for the life of a rotating board. The mode a station was PUT in and
  the network its radio is on this second are two different facts, and
  only the first belongs in NVS.
- **A task handed back and asked for again is a fragmentation bet you
  lose eventually.** Leaving `meshcore` mode stands the 6 KB `mcwork`
  stack down, which is right for an operator's mode change and wrong
  forty times an hour: "whoever starts last gets the fragments"
  (docs/esp32.md). A rotating station keeps the worker alive across the
  laps, which also means the sealing and signing for MeshCore's next turn
  is done while the radio is still on the other network. The one caller
  that genuinely wants those six kilobytes back, an OTA install, now says
  so with a flag the restart path honours, because the first version let
  a rotation take back what quiesce had just handed over.
- **A screen row is a memory decision.** The rotation began as its own
  Settings row, which is about 740 bytes once the row, the render scratch
  and the detail string are counted -- on a board with 12 KB free, for a
  feature that board cannot run at all, since MeshCore needs PSRAM. It is
  a fourth stop on the existing "LoRa mode" row instead: which networks
  this radio serves is one question, so it is one row.
- **Measure the thing itself, not what is lying around.** The first
  attempt at measuring the rotation's loss counted ambient bench traffic,
  and the transmitter aired one frame in three minutes (`lora_worth` keeps
  beacons off a mesh channel, correctly), so the ratio was noise over
  noise. The answer came from a transmitter under our control, one
  numbered packet every 12 s, counted by name at the far end.

## 16. Not done yet

- The P1-Pro (nRF52, RadioLib) still runs SF7 and is deaf to the fleet. It
  needs `begin()` on LongFast, `xlc_aes_encrypt_block()` over CC310 or
  mbedtls's `aes.c`, and the component by symlink (`library.json` is there).
- Positions and telemetry do not cross.
- A Meshtastic phone app cannot connect to an XPRS station as its radio:
  that would mean implementing Meshtastic's phone API (the Bluetooth
  service, `FromRadio`/`ToRadio`) on the station.
- XPRS stations show as offline in Meshtastic apps between NodeInfos (every
  `mt_ni_min`, 180 minutes), because Meshtastic only updates "last heard" for
  packets it can decode, and our XPRS frames are on a channel it cannot.
- The Heltec's minimum-ever heap wants a longer soak.
- `both` mode has not been run on a T-Deck with Bluetooth on: a fully loaded
  board is about 500 bytes short of `lr_claim_for`'s 22,528-byte internal
  reserve and refuses, exactly as it already does for plain `meshcore` mode on
  that build. Section 14 has the arithmetic. Either something comes off that
  board or `LR_MC_SPARE_INTERNAL` is revisited on purpose, and that is a
  decision about what the board is for.
- Nothing has been tried against a genuinely stock node of either network;
  there is none at this bench. Both protocols' traffic in section 14 came from
  our own stations put on the shared channel, which tests the classifier but
  not the other firmware's behaviour.
- Positions and telemetry do not cross in `both` mode either, for the same
  reason they do not in the single-network modes.
