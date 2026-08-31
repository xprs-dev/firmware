# Wi-Fi 8, and what of it this firmware can use

Somebody reads that Wi-Fi 8 gains range by hopping across channels, and asks the
obvious question: XPRS runs broadcast ESP-NOW, range is the thing that limits it,
so can we borrow that? This page is the answer, written once so it does not have
to be worked out again.

The short version: **802.11bn has no multi-hop, no relay and no mesh.** Its range
feature is a transmitter trick that needs radio silicon the ESP32 does not have.
Its reliability features are *coordination* between access points, and one of those
ideas does port. Meanwhile the multi-hop we were going to copy is already here, and
there is more of it than the standard defines.

[espnow.md](espnow.md) is the bearer's own page and [esp32.md](esp32.md) is binding
on how any of this gets measured.

## What Wi-Fi 8 is

802.11bn, marketed as Wi-Fi 8, is called **UHR** -- Ultra High Reliability -- and the
name is the whole design brief. **It does not raise the peak PHY rate.** The
[XDA piece][xda] that prompted this puts it plainly: approximately the same maximum
data rate as Wi-Fi 7. What it targets instead, at a given SINR, is 25% more
throughput, 25% less latency at the 95th percentile, and 25% fewer MPDUs lost.

Those are the numbers of a standard that has decided the headline speed is finished
and the connection is not.

## What Wi-Fi 8 is not

There is no hop in it. The "multichannel multi-hop" reading is two separate
mechanisms blurred together:

**Multi-AP Coordination (MAPC)** -- the C-family: coordinated spatial reuse (C-SR),
coordinated beamforming (C-BF), coordinated TDMA (C-TDMA), coordinated restricted
target wake time (C-RTWT). Access points agree, *over their own backhaul*, on who
transmits when and at what power, so that two nearby APs can transmit at the same
time instead of each deferring to the other. The client is still one hop from its
AP. Nothing is relayed.

**Non-primary channel communication** -- a device may talk exclusively on a
secondary channel while the primary is busy, rather than waiting for the primary to
clear. That is a second channel, not a second hop.

Add **seamless roaming** (handoff between APs with next to no downtime) and it is
easy to read the three together as "the network passes my packet along". It does
not. Every one of them is a single-hop mechanism with better manners.

## DRU, which is the actual range feature

The one thing in 802.11bn that genuinely buys range is the
**distributed-tone resource unit**, and it is worth understanding properly because
the reason we cannot have it is instructive.

Regulators cap not just total transmit power but **power spectral density** -- energy
per megahertz. A low-power indoor client is held to −1 dBm/MHz. A small station
assigned a narrow resource unit is therefore stuck: it occupies few megahertz, so the
PSD ceiling caps its *total* power far below what its amplifier could do, and its
uplink is the first thing to fail at distance.

A DRU takes that station's transmission and scatters its tones **non-adjacently**
across a much wider distributed bandwidth. The tones per megahertz go down; the
energy allowed in each tone goes up; the total radiated power rises while the PSD
ceiling is never crossed. Same regulation, more power, more uplink range -- and the
devices it helps most are exactly the class we build: sensors and small radios with
one antenna and nothing to spare.

**We cannot do it.** It is a subcarrier mapping decided in the transmitter. ESP-NOW
rides ordinary 802.11 b/g/n action frames and there is no API that places tones. This
is not a software problem that a clever driver call solves.

The same goes for C-BF and C-SR in their PHY sense: one antenna, no channel sounding,
no null-steering. Not available, not nearly available.

## What we already have

The mapping is unflattering to the premise. Set each UHR idea against this tree:

| Wi-Fi 8 idea | What XPRS already does |
|---|---|
| multi-hop | `xb_offer()` (`common/xprs_bearer/xprsbearer.c:117`) into `xprs_append_via()` (`common/xprs_codec/xprs.c:282`). Hop budget by type -- 9 for `sos` and `warning`, 3 for everything else (`xprs.c:218`). Loop prevention by `via:` membership (`xprs.c:244`). |
| multi-link (MLO) | Every packet is offered to every other bearer. `on_espnow` (`common/xprs_app/xprs_app.c:961`) fans to LAN and LoRa; `on_lan`, `on_lora`, `on_rns` fan back. One packet, three radios, §13.1 deciding each time. |
| non-primary channel | §23.7's `t:channel` rendezvous -- the whole of `common/xprs_chan/`, 888 lines and a 670-line host test. A pair leaves the calling channel for one of their own and comes home on a deadline. |
| link-state advertisement | Signed `t:observation f:X link:espnow peers:N hears:A,B`, fanned over every transmit lane -- `air_signed_observations()` (`xprs_app.c:1136`). Intake into a 32-slot gossip ring: `goss_note` (`:702`), `goss_try` (`:718`). |
| store and forward | Release-on-hearing, §36.8.1 (`xprs_app.c:856`): hear a station directly and its parked mail goes out on the bearer it was heard on. |
| duplicate suppression | Two 32-entry rings per bearer, 60 s TTL, keyed on the §5 identifier -- which ignores `sig:` and `via:`, so a relayed copy is the same packet (`xprsbearer.h:59-61`, `xprsbearer.c:164`). |
| contention avoidance | 200--1200 ms random re-air jitter, cancelled by hearing somebody else's copy (§13.2.1) -- `xprsbearer.h:55`, `xb_cancel` at `xprsbearer.c:57`. |

One gap is worth stating rather than glossing: the long-range PHY, below, is
written and unproven.

**`digi_on` defaults to TRUE since 2026-08-31.** It was false, and a stock
station bridged between bearers but never re-aired a medium onto itself --
which is half a relay. These boards exist to extend range across terrain, so
the default now matches the purpose: every bearer offers to every other, and
re-airs its own. The duplicate rings, the hop budget (13.1) and the cancel
window (13.2.1) are what keep that from becoming a storm, and they were
always the parts doing that work.

Ignore [mesh-networking.md](mesh-networking.md) when reading this page. It documents
`common/xprs_mesh/`, an ESP-WIFI-MESH tree mesh that no other component's
CMakeLists references and whose file paths still describe the layout from before this
tree moved. It is not the ESP-NOW path and it is not what the gossip above runs on.

## What the ESP32 actually has for range

One lever, and it is the long-range PHY. `xc_set_lr()`
(`common/xprs_chan/xprschan.c:152-180`) adds `WIFI_PROTOCOL_LR` to the
protocol bitmap and sets the broadcast peer to `WIFI_PHY_MODE_LR` with
`WIFI_PHY_RATE_LORA_250K`. Espressif claims about a kilometre line of sight,
ESP32 to ESP32 only, at a quarter of a megabit.

Those three lines are the only occurrence of `WIFI_PROTOCOL_LR` or `LORA_250K` in
`common/`, and they run only inside a §23.7 excursion -- never on the calling channel.

**It has never been measured.** Both calls return `ESP_OK` and that is the entire
basis for believing it does anything. Reading the PHY back and comparing delivery
against the normal rate at two distances is the first item in [TODO.md](../TODO.md),
written up in full there; it is not repeated here. Until that table is filled in,
"LR gives us range" is an assumption wearing a log line, and no design should be
built on top of it.

Transmit power is not a second lever. The XPRS station path never calls
`esp_wifi_set_max_tx_power` at all -- the only call in the tree is in the SoftAP path
(`common/xprs_wifi/wifi_bsp.c:377`, 80 = 20 dBm). The station inherits the driver
default, and moving it is a regulatory question rather than a free win.

## The idea that does port: coordinated relay election

This is the MAPC analog, and it is the one thing on this page worth building.

**What happens today.** Several stations hear the same relayable packet. Each rolls
its own uniform 200--1200 ms delay (`xb_queue_push`, `xprsbearer.c:88`) and queues a
re-air; the first to fire wins and hearing it cancels everybody else's copy
(`xb_cancel`, `:57`). It works, and it has two costs. The winner is **random** -- not
the station whose re-air would reach furthest -- and until the cancel lands, several
stations have each spent the airtime of deciding.

That is exactly the deferral waste MAPC removes. The difference is that access points
coordinate over a backhaul and we have nothing but what we already hear. Which turns
out to be enough.

**The proposal.** Keep the jitter mechanism entirely; stop making it uniform. Derive
each station's delay from a **rank it can compute alone**, so that the station best
placed to extend the packet's reach draws a short delay and the redundant ones draw a
delay long enough to be cancelled before they ever transmit.

The rank inputs are all already in RAM:

- **RSSI of the heard copy.** A *weak* copy is the interesting one -- it means this
  station sits at the edge of the originator's range, which is precisely the station
  whose re-air covers ground the originator does not. A station that heard the packet
  loudly is standing next to the sender and adds nothing.
- **The gossip ring** `s_goss[32]` (`xprs_app.c:696`) and the per-bearer `hears:` list
  (`xst_hears_render`, `common/xprs_station/xprs_station.c:295`). A station that hears
  callsigns the originator does not is a bridge. One that hears only what the
  originator hears is a duplicate with a different MAC.
- **Local degree** from the 16-slot peer table (`xb_peer_touch`, `xprsbearer.c:144`).

Two rules keep it from failing in the obvious ways. Tie-break by hashing the §5
identifier together with the local callsign into the low bits of the delay --
deterministic per packet, different per station, so equal-ranked stations still
separate instead of colliding. And keep a floor of randomness, so a station that
happens to rank badly for structural reasons is not muted forever.

**Why this shape and not another.** No new XPRS key, no new packet type, no change to
the wire at all: the entire mechanism is a change to how one local number is chosen.
That is not modesty, it is the constraint doing its work. `sig:` covers the packet
with only `sig:` and `via:` stripped (`common/xprs_sig/xprssig.h:21-26`,
`xprs_signed_text` at `xprs.c:192`), and that skip list is mirrored in the Dart
reference implementation. **Any new hop-count, TTL, relay-flag or sequence field
either invalidates every signature at every hop, or forces a matching change to
`encode_ex()` (`xprs.c:124`) in two repositories.** Most of the obvious designs die
right there, and the one that survives is the one that never touches the packet.

**One thing it does need.** RSSI reaches `on_espnow` (`xprs_app.c:961`) and is dropped
before the relay decision: `xprsnow_offer(wire, len)` (`xprsnow.c:193`) calls
`xb_offer(wire, len)` with no signal argument. Plumbing it through those three
signatures is small, but it is real work and it touches the shared bearer interface,
so every bearer has to answer the question -- the LAN has no RSSI to give, which
`xb_rx_cb_t` already handles by convention (`xprsbearer.h:64-70`: zero for a bearer
with no signal to report).

**What it is not.** Not a routing protocol. Nothing propagates, no metric is
advertised, no state is shared. One station, one packet, one local decision -- the
same shape as the jitter it replaces, which is why it can be tested on a host in
`xprs_bearer` before any of it reaches a radio.

### Considered and deferred

**Channel-lane diversity** -- the C-SR and non-primary-channel analog: air the same
packet on two or three channels in rotation, or have neighbours agree to sit on
different ones. It contradicts the rule this bearer is built on -- two devices on
different channels hear nothing from each other and nothing reports an error
(`docs/espnow.md`, "Same channel, or nothing at all") -- and it multiplies airtime for
a reliability gain nobody has measured a need for.

**Duty-cycled listen scheduling** -- the C-RTWT analog: neighbours agree on wake
windows so a re-air lands while the next hop is listening. It is a power and latency
mechanism rather than a range one, and it fights `WIFI_PS_NONE` (`xprsnow.c:277`),
which is set deliberately because a sleeping station misses frames.

## Constraints anything built here inherits

Collected so the next attempt does not rediscover them.

- **`sig:` covers everything but `sig:` and `via:`.** The big one, above.
- **`PEERKEYS_MAX 4`** (`xprs_app.c:156`) -- first speaker wins, never evicted. A relay
  cannot verify signatures from more than four distinct stations.
- **`XB_TICKED_MAX 4`** (`xprsbearer.c:260`) -- four bearer slots total, and lan,
  espnow, lora and ble already contend for them.
- **One driver task.** `xprslan`'s 7168-byte task pumps every registered bearer every
  100 ms (`common/xprs_bearer_lan/xprslan.c:165`, `:230`). No LAN bearer means ESP-NOW
  never re-airs and never beacons; `xprsnow_start()` logs an error rather than let that
  be discovered in the field.
- **Heap is the binding constraint everywhere.** The ESP-NOW receive queue is 8 slots
  because 16 starved `esp_now_send` into `ESP_ERR_ESPNOW_NO_MEM`
  (`common/xprs_bearer_now/xprsnow.h:63-79`).
- **`XB_SEEN_MS` is 60 s** (`xprsbearer.h:61`), which bounds how long a multi-hop path
  may take before a packet may legitimately circulate again.
- **BLE deafness.** With the BLE controller running, a station that is not associated
  receives *nothing* while transmitting perfectly -- the measured truth table is in
  [espnow.md](espnow.md). Any persistent off-AP lane inherits it.
- **Leaving the access point stops the iGate, the Reticulum hub and the clock.** §23.7
  tolerates that only because `XC_MAX_AWAY_MS` is 45 s, local, and non-negotiable:
  `until:` can shorten the stay and never extend it (`xprschan.h:21-33`).

## References

- [Wi-Fi 8 is the first wireless upgrade in years that isn't chasing speed][xda] -- XDA
- [Spatial Reuse in IEEE 802.11bn Coordinated Multi-AP WLANs: A Throughput Analysis](https://arxiv.org/abs/2407.16390)
- [IEEE 802.11bn Multi-AP Coordinated Spatial Reuse with Hierarchical Multi-Armed Bandits](https://arxiv.org/abs/2501.03680)
- [Wi-Fi 8 Prioritizes Consistent Connectivity Over Peak Speed](https://www.litepoint.com/blog/wi-fi-8-prioritizes-consistent-connectivity-over-peak-speed/) -- LitePoint
- [IEEE 802.11bn (Ultra-High Reliability)](https://research.samsung.com/blog/IEEE-802-11bn-Ultra-High-Reliability-UHR-Wi-Fi-8) -- Samsung Research
- [RRU vs DRU in Wi-Fi 8](https://www.rfwireless-world.com/terminology/wi-fi-8-rru-vs-dru-differences)

[xda]: https://www.xda-developers.com/wi-fi-8-first-wireless-upgrade-years-isnt-chasing-speed-home-networks-need-it/
