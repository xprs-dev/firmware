# Meshtastic on the XPRS LoRa radio

Since 2026-09-19 every XPRS station with a LoRa radio runs on Meshtastic's
default channel and does three things there at once:

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

## The rules we follow (adopted 2026-09-19)

These are decisions, not descriptions. Code that breaks one of them is a
regression even if it works on the bench; each one has already cost a bug.
The app half of the same rules is in `app/docs/meshtastic.md` section 5.

**Addressing and reach**

1. **A Meshtastic node is `MT` plus its node number in eight uppercase hex
   digits** (`MC` for MeshCore, reserved). Never an `X` class. Every surface
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

**The radio and the tasks**

11. **LoRa carries only what somebody waits for** (`lora_worth`): messages,
    receipts, reactions, sos, warnings, commands, results, identities,
    mailboxes, files, requests. Presence stays on the cheap bearers.
12. **Nothing slow runs on the LoRa task or under the bridge's mutex.** The
    curve work runs on the bearer tick; translations and receipts are parked
    by `mesh_deliver` and sent from `idx_task` on core 1. Every other
    bearer's task (the Bluetooth host among them) waits on that mutex to offer
    the bridge a packet.
13. **A DM retried after the recipient could not open it goes under a new
    id.** A Meshtastic node records an id as seen even when it fails to
    decrypt, and drops every repeat of it silently.
14. **Counted where it can be read.** `/api/status` carries the bridge's
    counters under `lora.mt`; a new behaviour gets a counter there the day
    it is written.

## The channel

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

## XPRS on the shared channel

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

## What a station puts on LoRa

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

## Identities

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

## Direct messages

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

## Broadcasts, replies, likes, names

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

## The repeater

A frame not heard before (dedup on from and id, ten minutes), with hops left,
not ours and not for one of our nodes, and whose `next_hop` is 0 or ours, is
re-aired with `hop_limit - 1` and our relay byte, after the firmware's CLIENT
wait: `2 x 8 x slot + random(0, 2^cw) x slot`, the slot 28 ms at LongFast and
`cw` 3 to 8 from the SNR, so a faint copy goes first. Hearing somebody else
relay it cancels ours. Frames are relayed whether or not they can be read.

## The radio

- **Listen before talk**: a header already arriving (the `HEADER_VALID` latch,
  cleared if older than 2.5 s), then SX1262 channel activity detection (two
  symbols, about 20 ms). An XPRS packet waits up to eight slots and then goes;
  a Meshtastic frame is simply retried later.
- **Transmission does not block**: `sx1262_tx_start()` returns once the frame
  is in the FIFO, and the next bearer tick puts the radio back in receive.
  The bearer task pumps every bearer, and SF11 frames are two seconds.
- **One budget**: Meshtastic frames spend the same duty ledger as XPRS
  (`xb_spend()`), and a priority frame may use the reserve.

## Configuration

`config.ini`, and `cfg set` over serial:

| section | key | NVS | default | |
|---|---|---|---|---|
| `[lora]` | `local` | `lora_local` | `no` | LoRa counts as a local bearer (9.11.1) |
| `[meshtastic]` | `repeat` | `mt_repeat` | `yes` | relay Meshtastic frames |
| | `bridge` | `mt_bridge` | `yes` | translate both ways; adds `meshtastic` to `serve:` |
| | `broadcasts_per_hour` | `mt_bcast_hr` | 12 | XPRS broadcasts mirrored onto LongFast |
| | `nodeinfo_min` | `mt_ni_min` | 180 | how often our nodes re-announce |

## Memory and stack

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

## Measured on the bench, 2026-09-19

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
| desktop chat wapp DM to an unheard node (`MT0BADCAFE`) | left by the T-Deck: `dm_not_here` counted, `key_asks` 0, nothing aired |
| desktop chat wapp DM to the witness | delivered (`text_out` 1, `dm_acked` 1), shown in the phone app; the Heltec's receipt released the desktop's held copy |

## Lessons learned

Each of these cost at least one wrong turn on 2026-09-19. Read them before
changing the bridge.

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
- **A radio that is not advertising is not a range problem.** The witness
  had Bluetooth off in its config; the phone app saw nothing until
  `bluetooth.enabled` was set.

## Not done yet

- The P1-Pro (nRF52, RadioLib) still runs SF7 and is deaf to the fleet. It
  needs `begin()` on LongFast, `mt_aes_encrypt_block()` over CC310 or
  mbedtls's `aes.c`, and the component by symlink (`library.json` is there).
- Positions and telemetry do not cross.
- A Meshtastic phone app cannot connect to an XPRS station as its radio:
  that would mean implementing Meshtastic's phone API (the Bluetooth
  service, `FromRadio`/`ToRadio`) on the station.
- XPRS stations show as offline in Meshtastic apps between NodeInfos (every
  `mt_ni_min`, 180 minutes), because Meshtastic only updates "last heard" for
  packets it can decode, and our XPRS frames are on a channel it cannot.
- The Heltec's minimum-ever heap wants a longer soak.
