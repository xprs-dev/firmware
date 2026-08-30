# Connections, and getting 1:1 off the broadcast plane

Why a station that has something to say to *one* other station should open a
connection rather than shout, what was removed when `tinynimble` replaced
NimBLE, and what it would cost to put back.

## The premise is right, and here is the mechanism

Advertising is a broadcast on **three** channels — 37, 38 and 39 — shared by
every BLE device in the room, ours and everyone else's. A connection runs on
the other **thirty-seven**, with adaptive frequency hopping, and is heard by
nobody but the two ends.

So a 1:1 message carried as adverts costs three things, and only the first is
obvious:

1. **It occupies the shared channels.** `ble5.md` §1: there is one
   advertising set and it is a *window*, not a state — airing a 1:1 payload
   means the discovery beacon is not being aired, on this station and, once
   the channels are busy, on the ones nearby.
2. **Every listener in range pays.** An advert reaches everybody. Each one
   copies it, derives a §5 identifier — a SHA-256 — checks two duplicate
   rings and drops it. For a conversation between two stations, that work is
   wasted on every other station within earshot.
3. **It scales the wrong way.** Bulk especially: a file moved chunk-by-chunk
   over adverts puts the entire transfer on the plane everyone shares.

A connection turns all three into a private cost between the two ends. This
is the correct instinct and the rest of the page is about what it takes.

## What was actually removed — less than it looks

When the T-Dongle retired NimBLE for `tinynimble`, its `sdkconfig` said:

> GATT goes with it. The connectable instance, the FFE0/FFF1/FFF2 service and
> the MSP bulk path are retired — this board broadcasts and listens, like
> every other XPRS station.

That retired `models/tdongle-s3/firmware/src/gatt_mesh.c` — a GATT **server**
built on NimBLE's stack, living in one board's `main.c` tree where no other
board could reach it.

**The protocol above it was never removed and is still here.**
`common/xprs_blemesh/blemesh_session.c` is the Mesh Session Protocol: custody
transfer, gossip, chunked bulk with windowed acks. It is a C mirror of
aurora's `mesh_session.dart`, the two codecs are byte-identical against
shared hex fixtures, and `test_msp_host.c` compiles it on a desk.

And it is **already transport-agnostic**. The entire contract a link has to
satisfy is one function:

```c
/* Queue one MSP frame on the link. BLEMESH_SEND_OK, BLEMESH_SEND_BUSY, ... */
int (*send)(void *ctx, const uint8_t *frame, int len);
```

plus feeding received bytes to `blemesh_session_rx()` and calling
`blemesh_session_tx_ready()` when the queue drains. Everything else in
`blemesh_session_ops_t` — custody, gossip, bulk spooling — is the
application's storage, not the transport's business.

**So this is not "rebuild the mesh". It is "give an intact protocol a link
to run on."** That is the single most useful fact on this page.

## What tinynimble would have to grow

Today it is deliberately six HCI commands and one event — broadcaster and
observer, 769 lines across three files, no connections at all.

Two constants in `tinynimble.h` are already defined and used by nothing:

```c
#define TN_H4_ACL                0x02    /* the ACL packet type */
#define TN_ADV_PROP_CONNECTABLE  0x0001  /* the advertising property */
```

The door was left open on purpose. Walking through it needs three layers:

**1. HCI — connections.** Connectable extended advertising (the property
above), `LE Enhanced Connection Complete`, `Disconnection Complete`, and ACL
data in and out — which is the first time this stack carries a packet type
other than command and event. Optionally `LE Set Data Length` and
`LE Connection Update`, both of which are one command each and both of which
matter for throughput.

**2. L2CAP — almost nothing.** ATT rides fixed channel `0x0004`. A fixed
channel is a four-byte header, length and CID. No signalling channel, no
dynamic channels, no connection-oriented channels. This layer is tens of
lines, not hundreds.

**3. ATT — a server, with the table hard-coded.** This is the real work, and
it is bounded because the service is known at compile time: FFE0 with FFF1
(notify) and FFF2 (write), the same channel the phones already speak. A fixed
attribute table needs `Exchange MTU`, `Find Information`, `Read By Group
Type` and `Read By Type` (so a client can discover it), `Read`, `Write`,
`Write Command`, `Handle Value Notification`, and `Error Response`.

Estimate: **700–900 lines**, in the same shape as `tn_hci.c` — encode and
decode over caller-owned buffers, no heap, host-testable byte-for-byte on a
desk before any of it goes near a radio.

### It does not bring back the 37 KB

`tinynimble/README.md` measured NimBLE's host at **37,760 bytes of text with
zero callers** — but that is a *general* host: a security manager, bonding
storage, dynamic GATT registration, L2CAP connection-oriented channels, a
client as well as a server. A single hard-coded service with no pairing is a
fraction of it, and the 22 KB of heap the retirement bought back was mostly
msys mbuf pools and the 5,120-byte host task, neither of which a fixed-table
server needs at NimBLE's scale.

The honest position: some of the saving comes back, and how much is a
measurement to take rather than a number to promise. `tools/tinynimble_probe/`
is the harness that took the first one.

## Four constraints to design against

**One advertising set, and a station needs two things from it.** The XPRS
discovery beacon and a connectable presence advert cannot both be up on a
chip with one set. The nRF52840's S140 has `BLE_GAP_ADV_SET_COUNT_MAX = 1`,
an absolute (`ble5-nrf52.md`), and the T-Deck currently configures
`CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES=1` even though its controller could
do more. So the default answer is **time-sharing**, which `ble5.md` §1
already prescribes for the beacon rotation — a connectable window offered
periodically, not held open.

**A connected station is partly deaf.** The radio cannot listen while it
talks, and a connection has scheduled events it must honour. Time spent in a
session is time not scanning the broadcast plane. A 1:1 exchange should
therefore be *time-boxed* and closed, not kept open for convenience — which
is what MSP already does, it moves custody and finishes.

**Whoever dials must be nameable.** The value of moving 1:1 off the broadcast
plane is lost if a station has to advertise connectably to everyone all the
time. §23.7's working-channel rendezvous is the existing pattern for "we two
have agreed to meet elsewhere" and is the natural place to negotiate a
connection window rather than inventing a second mechanism.

**A writable characteristic is an attack surface on a roof.** The broadcast
plane is unauthenticated by design and the packets carry their own
signatures — `docs/XPRS.md` §9.1 — and the transport must not be trusted any
further than that just because it is point-to-point. A connection is
*private*, not *authentic*. Verify the same way, or the connection becomes
the soft way in.

## Measured, 2026-08-30

Steps 2 to 5 below are done and the whole chain is measured on the bench:
a T-Deck running `tools/tinynimble_probe` (tinynimble over HCI, serving
`tn_att`, its beacon set made connectable) and the SenseCAP P1-Pro
(tinynimble's SoftDevice port, dialling). Fixed handles, so no discovery:

```
CAP   dial: X24CA at ca:24:3c:0c:da:dc
DECK  link up 0x0002
CAP   tn: mtu 247 -- gatt: link ready (we dialled), 244 bytes per send
CAP   'm'  ->  DECK gatt rx 20B: hello from X54W6W #1
           ->  CAP  gatt rx 25B: echo:hello from X54W6W #1
CAP   'M'  ->  DECK gatt rx 244B: ABCDEF...  ->  CAP gatt rx 244B: echo:ABCDEF...
DECK  'm'  ->  CAP  gatt rx 16B: hello from X24CA
CAP   'x'  ->  DECK link down 0x0002 (0x13) / CAP link closed (0x16)
CAP   'd'  ->  link up again, MTU 247, traffic again
```

Three things the bench taught that the design above did not predict:

- **An advertising set cannot be reconfigured while enabled** -- the
  controller answers `0x0C` Command Disallowed. `tn_adv_configure()` now
  disables first.
- **`CONFIG_BT_CTRL_BLE_MAX_ACT` must be 3, not 2**, for a connectable
  set beside a scan: the controller reserves an activity for the link the
  set may produce, and without it the enable answers `0x07` Memory
  Capacity Exceeded on every beacon. The old T-Dongle GATT config ran 3.
- **While the link is up the connectable set cannot be re-enabled** at
  all (same `0x07`), so the port sets the beacon data and leaves the set
  down until the link drops, then enables it from the pump. A station in a
  session is silent on the broadcast plane -- which is the argument for
  short sessions made above, now enforced by the hardware.

What is still open: `xprs_bearer_ble` does not yet call `tn_gatt_serve()`,
so the station firmware does not offer the channel -- only the probe does;
and the SoftDevice port does not serve (`tn_gatt_serve` answers
`NRF_ERROR_NOT_SUPPORTED`), so two nRF52 boards cannot yet connect to each
other. The MSP session (`blemesh_session.c`) has not been wired to
`tn_gatt_send()`; that is step 4 below and is now unblocked.

## Suggested order

1. **Measure the premise.** Put a 1:1 exchange on the broadcast plane and
   count what it costs the stations that are not party to it. `docs/esp32.md`
   is binding on how; the number makes the rest of the case, or unmakes it.
2. **`tn_att.c`, on the desk first.** The attribute table and the PDU codec,
   with a host test that asserts every byte, exactly as `tn_hci.c` was built.
   No radio involved.
3. **ACL and the fixed L2CAP channel in `tn_port_esp.c`**, then a connection
   between two T-Decks that does nothing but exchange MTU.
4. **Wire `blemesh_session_ops_t.send` to a notification** and let the
   existing MSP move one message. The protocol is already proven against the
   phone's implementation, so a successful custody transfer is a real result
   on the first try.
5. **Only then the nRF52840**, where the same ATT and L2CAP code sits on the
   SoftDevice's connection instead of on HCI — and where the single
   advertising set makes the time-sharing question unavoidable.

Steps 2 to 4 are all ESP32 and all testable with hardware already on the
bench.
