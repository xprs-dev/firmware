# SenseCAP Card Tracker T1000-E

A credit-card tracker made into an XPRS station that a person carries. It is
the network's pocket relay: it hears the people and stations around it on
Bluetooth, carries what matters onto whichever LoRa mesh is in the area,
repeats that mesh's own traffic, says where it is and how warm and how full
it is, and holds mail for the people it meets until it meets them again.

Callsign prefix **X2** (a movable station, XPRS.md section 3). The bench card
is `X2V24Q`.

## What it does

**Finds the mesh.** At boot it listens on Meshtastic's LongFast for up to
20 s and on MeshCore's default for up to 8 s, asking each with a probe that
the network's repeaters carry (`common/xprs_bearer_lora/lr_probe.c`). A
probe carried back beats an XPRS station heard, which beats any frame heard,
which beats nothing; with nothing anywhere it keeps the network it remembered
from last time. Every hour it asks again, the other network first and then
its own, and moves only when two sweeps in a row agree
(`lr_detect.c`, host test `test_detect_host.sh`). The verdict is kept in
flash across a restart.

**Carries XPRS both ways.** Everything heard on LoRa goes to Bluetooth.
From Bluetooth to LoRa it is choosier, because the channel is shared and
every frame is a second of transmitter: only what docs/lora.md rule 11 calls
worth LoRa (`lr_worth.c`), never `scope:local`, never a packet for somebody
in the same room, never a carousel or replay more than half an hour old,
nothing twice in half an hour, station-to-station commands and results only
toward a station heard on LoRa, and a phone's identity once in three hours.
On the bench this took the card from 38 frames in three minutes to three.

**Repeats the native mesh.** `lr_repeat.c` is the managed flood of
Meshtastic and the flood repeater of MeshCore, and nothing else: no bridge,
no node keys, so no X25519 and no Ed25519 (the full `mt_mesh`/`mc_mesh`
would not fit the 256 KB update slot). Repeats share one airtime ledger
with XPRS (10% of an hour in band g3, with a reserve for sos).

**Says where it is.** It asks an owner's phone in Bluetooth reach
(`t:request q:pos`) and takes the answer only from that phone, signed,
while the phone itself is heard directly, within two minutes (a phone advertises five seconds of every
minute, so an answer waits for its next window: 87 s on the C61). With no such phone it powers its own receiver
for one fix (45 s hot, 120 s cold), and two fixes in the same place make it
"still", which stretches the period from 15 minutes to an hour. A position
older than its freshness is left out: XPRS has no "this is how old" key, and
absence means unknown (15.1).

**Beacons.** On Bluetooth every five minutes, the whole of it:
`pos: acc: temp: batt: volt: mail: hears:`. On LoRa every fifteen, lean: no
`volt:` and only the most recent few of `hears:`. `t:identity` every 30 min
on Bluetooth and every 3 h on LoRa; `t:service serve:relay site:portable
source:battery` hourly on Bluetooth and every 3 h on LoRa.

**Is the preferred station of the people it meets.** Its beacons are signed
and carry `hears:` with `link:ble`, which is what writes it into the L2 visit
history of every archiver that hears them (XPRS.md 12.9.4): the network
learns, without anybody configuring anything, that mail for those people
should come through this card. The owner's phone also names its card first
in its `t:mailbox hold:` (9.12).

**Stores and forwards.** `common/xprs_mailbox` on the 24 KB of flash at
0xE7000: 64 records, class 3 for callsigns whose verified `t:mailbox` names
the card, class 2 for mail from or to its contacts of the last 30 days. On
hearing the recipient directly it hands over a page of three on that bearer,
newest first, backing off 30 s / 2 min / 10 min on the same copy; a verified
receipt releases the copy on flash before it counts. `q:mail` is answered
(`mail:0` is an answer). Keys for verifying come from the owners and from
`t:identity` packets whose callsign derives from their own key.

## Power

The target is three days on one charge. The design figure, before a soak
has measured it, is about 5 mA:

| | |
|---|---|
| nRF52840 asleep between events (tickless idle; the loop sleeps 250 ms or until LoRa, Bluetooth or the console wakes it) | 0.1 mA |
| Bluetooth scan, 10% base and 90% in a phone's window | ~1 mA |
| Bluetooth adverts, 1 s at 0 dBm | 0.02 mA |
| LR1110 duty-cycled receive (4 symbols of a 16-symbol preamble) | ~2.3 mA |
| LoRa transmit at +20 dBm, beacons, relays and repeats | ~1 mA |
| receiver fixes, only without a phone | <0.2 mA |

DC/DC on the nRF52840 is left off: nothing (Meshtastic's variant included)
shows the card has the inductor, and enabling it without one stops the chip.

## Console

The USB console prints only with DTR asserted (open it with pyserial, not
`cat`). One letter each:

| | |
|---|---|
| `?` | one line of state |
| `k` / `K` / `I` | callsign, nsec, import a key |
| `H` | radio version, LF clock, DC/DC, bootloader and settings page, files |
| `S` | temperature, battery, charger |
| `w` / `g` | position state / power the receiver for a fix now |
| `m` / `T` | mail counters / trace why each packet was or was not held |
| `n` / `d` / `p` | switch network / sweep now / air one probe |
| `r` | duty-cycled or continuous receive, for measuring |
| `b` | scan base or boosted |
| `X` + line | take a typed packet as heard on Bluetooth (bench tests) |
| `c` + `fg set own1 <npub>` | configuration; writes and reboots |
| `D` | reboot into the bootloader |

Owners are set by cable: `own1` is the owner's npub and `own1c` its callsign.
There is no over-the-air claim on the nRF52 stations yet.

## Lessons this card taught

- **The LR1110 keeps the transmit length as the receive limit.** RadioLib
  re-sends the packet parameters on receive only in implicit-header mode, so
  after a 6-byte probe the radio dropped everything longer, including the
  7-byte relay of that probe. `lora_tx_frame` re-sends them after every
  transmit (`setPreambleLength`).
- **`startReceiveDutyCycleAuto` needs `minSymbols` given.** RadioLib's
  default of 8 against a 16-symbol preamble leaves less sleep than the
  radio's own transition time, and it quietly falls back to continuous
  receive.
- **Seed the generator from the hardware.** `randomSeed(DEVICEID)` made every
  boot's probe byte-identical, and the repeaters dropped it as a duplicate of
  the last boot's.
- **Reset on the AG3335 is active high.** Meshtastic's `GPS_RESET_MODE HIGH`
  is the level that resets it.
- **The bootloader's settings page is at 0xFE000** (UICR NRFFW[1]), not the
  XIAO's 0xFF000, and on this card it is erased: no CRC is recorded for the
  application.
- **The core's uf2conv.py ignores hex type-02 records** and put the image at
  0x2000, over the SoftDevice. `scripts/post_uf2.py` converts the binary with
  `-b 0x27000`.

## Not done yet

The accelerometer (motion would gate the receiver better than "two fixes in
the same place"), the light sensor, an over-the-air claim, the signed update
exercised on this card, and the 72-hour battery soak that turns the table
above into a measurement.

## Back to Meshtastic

Send `D` on the console (or touch the port at 1200 baud) to start the
bootloader, and copy Meshtastic's T1000-E UF2 onto the `T1000-E-BOOT`
volume. This firmware leaves the bootloader alone.
