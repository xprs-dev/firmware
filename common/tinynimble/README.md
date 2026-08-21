# tinynimble

The BLE5 surface XPRS uses, spoken straight to the controller. No NimBLE host.

**Working on hardware.** Validated on two T-Decks, 2026-08-21.

## Why

NimBLE gives this firmware six things it needs and a great deal it does not.
Measured on the T-Dongle-S3 image, the host's security manager, ATT/GATT, L2CAP
and bonding store are **37,760 bytes of text with zero callers anywhere in the
tree** — the security manager is only enabled to work around an IDF 5.2.1
compile bug. What the stations actually do is broadcast and listen.

Measured **on the same board, same test, only the stack changed**
(`tools/tinynimble_probe`, T-Deck, 2026-08-21). NimBLE here is already trimmed
the way the fleet trims it — observer + broadcaster, no central, no peripheral,
MSYS 4/2 — so this is NimBLE at its smallest, not a strawman.

| | tinynimble | NimBLE | saving |
|---|---|---|---|
| flash code | 196,987 | 214,339 | **17,352** |
| flash data | 62,564 | 66,164 | **3,600** |
| static `.bss` | 3,744 | 6,608 | **2,864** |
| **heap at BLE init** | **28,288** | **47,396** | **19,108** |
| steady-state free heap | 340,360 | 318,060 | **22,300** |
| largest free block | 270,336 | 253,952 | 16,384 |

**≈21 KB of flash and ≈22 KB of RAM.** On the T-Dongle-S3, whose steady-state
free heap is around 14,304 bytes, the RAM half is not an optimisation — it is
more than doubling what the board has to work with.

Against the T-Dongle's *untrimmed* NimBLE (GATT, SM and peripheral role linked)
the flash difference is far larger — `libbt.a` is 64,632 bytes of text there
versus `libtinynimble.a`'s 2,282 — but most of that gap closes by trimming
roles in Kconfig alone, which costs nothing and should be done first regardless.
The honest figure for tinynimble's own contribution is the table above.

The **controller stays**, and its ~28 KB of internal heap with it. That is a
binary blob with microsecond deadlines; nothing here changes it. tinynimble
removes the *host's* share: the msys mbuf pools, the ACL transport buffers and
the 5,120-byte host task stack.

## What it is

Six HCI commands and one event, plus the three-command bring-up that nobody
mentions until nothing arrives:

| opcode | |
|---|---|
| `0x0C03` | Reset |
| `0x0C01` | Set Event Mask — **must include LE Meta (bit 61)** |
| `0x2001` | LE Set Event Mask — **must include Extended Advertising Report (bit 12)** |
| `0x2005` | LE Set Random Address |
| `0x2035` | LE Set **Advertising Set** Random Address |
| `0x2036` | LE Set Extended Advertising Parameters |
| `0x2037` | LE Set Extended Advertising Data |
| `0x2039` | LE Set Extended Advertising Enable |
| `0x2041` / `0x2042` | LE Set Extended Scan Parameters / Enable |

Event: `LE Meta → Extended Advertising Report` (subevent `0x0D`). Nothing else.

## Two traps, both paid for on the bench

**`0x2035` is not `0x2005`.** An *extended* advertising set carries its own
random address. Configure a set with `own_addr_type = random`, skip `0x2035`,
and the controller accepts the parameters, accepts the data, then refuses
`0x2039` with `0x12` "Invalid HCI Command Parameters" — blaming the enable, not
the missing address.

**Both event masks default to excluding what a scanner needs.**
`Set_Event_Mask` defaults to bits 0..44, so **LE Meta (61)** — the envelope
every LE event travels in — is off. `LE_Set_Event_Mask` defaults to bits 0..4,
so **Extended Advertising Report (12)** is off. With both masked the controller
accepts scan parameters and a scan enable, reports **no error**, and delivers
nothing at all. That symptom is indistinguishable from a dead antenna.

## Layering

- `tn_hci.c` — encode/decode over caller-owned buffers. **No ESP-IDF, no heap,
  no tasks.** The bytes that reach the radio are checked on a desk:
  `test/test_tn_host.sh` asserts every command byte-for-byte and feeds the
  decoder truncated and over-claiming packets on purpose.
- `tn_port_esp.c` — the only file that knows about `esp_bt_controller` and
  `esp_vhci`. One semaphore, one command buffer, one advert staging buffer.

**The report callback runs in the CONTROLLER's context.** Parse, identify,
deduplicate, park. Nothing that blocks, allocates without bound, or takes a lock
another task holds — the rule `docs/esp32.md` states for every receive path
here, stricter in this one because the context belongs to the link layer.

## Measured on two T-Decks

`tools/tinynimble_probe/` — one binary, both decks, roles from MAC. Build the
reference stack with `PROBE_NIMBLE=1 pio run -e deckA_nimble`.

**tinynimble ↔ tinynimble, 32 s:**

| deck | heard the other | third-party XPRS |
|---|---|---|
| A `XF656` | `X24CA` ×162, −65 dBm | `X1A67X` ×325 |
| B `X24CA` | `XF656` ×154, −65 dBm | `X1A67X` ×337 |

**tinynimble ↔ NimBLE** — the one that proves the wire format, deck A on the
reference stack and deck B on this one:

| deck | stack | heard the other |
|---|---|---|
| A `XF656` | NimBLE | `X24CA` ×164, −64 dBm |
| B `X24CA` | tinynimble | `XF656` ×167, −68 dBm |

Both directions, different stacks. The bytes are the same bytes.

Both also decoded live XPRS protocol frames from a station on the bench —
`t:identity f:X1A67X ts:…`, `t:observation f:X1A67X link:ble`, `X1A67X?HELLO` —
so this parses the real ecosystem's adverts, not only its own.

**Lifecycle**, the case that matters for ESP-NOW working-channel moves
(`docs/espnow.md`: with the controller up, an unassociated WiFi station receives
nothing, and cancelling the scan does not give the radio back):

```
tn_stop()  -> controller down (disable ESP_OK, deinit ESP_OK)
heap       -> 340,360 -> 368,268   (+27,908 B returned to internal)
8 s down   -> reports frozen: genuinely deaf, radio fully released
tn_start() -> up, ESP_OK, scanning resumes
12 s later -> heard the peer deck again, 158 frames
```

## Not done yet

- Not wired into the firmware. `common/geogram_xprsble/xprsble.c` is the
  intended first consumer — it already wraps advertise+scan behind one callback.
- Reachability under WiFi load not measured — the probe runs no WiFi, so the
  coexistence question `docs/espnow.md` documents is still open for this stack.
- No legacy advertising, no connections, no GATT. Deliberately: those are being
  retired fleet-wide in favour of the broadcast-only shape.
