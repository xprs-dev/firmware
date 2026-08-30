# BLE5 on the nRF52840

How the SenseCAP P1-Pro gets the BLE5 bearer, and why the obvious route does
not work. Read `ble5.md` first — this page only covers what is different
about the chip.

## The finding: Bluefruit cannot carry this bearer, in either direction

The Adafruit nRF52 Arduino core ships Bluefruit, and Bluefruit is hard-capped
at the legacy 31-byte advert. Not "awkward to extend" — sized at compile time,
in both directions, with no path through the API:

```
libraries/Bluefruit52Lib/src/BLEAdvertising.h:75
    uint8_t _data[BLE_GAP_ADV_SET_DATA_SIZE_MAX];      /* = 31 */

libraries/Bluefruit52Lib/src/BLEScanner.h:100
    uint8_t _scan_data[BLE_GAP_SCAN_BUFFER_MAX];       /* = 31 */
```

The string `extended` does not appear in `BLEAdvertising.cpp` or
`bluefruit.cpp` at all. An XPRS beacon runs to 112–173 bytes and
`XPRSBLE_WIRE_MAX` is 248, so this is not a matter of tuning a constant: the
station could neither air a packet nor hear one.

**This is worth stating plainly because it is easy to get wrong.** The board's
marketing says Bluetooth 5.0, the chip is a Bluetooth 5 controller, and the
SoftDevice is a Bluetooth 5 host. All true, and none of it reaches the sketch
through the library the core hands you.

## The SoftDevice can, and it is right there

S140 v7.3.0 ships with the core and supports the whole of extended
advertising. From its own headers
(`cores/nRF5/nordic/softdevice/s140_nrf52_7.3.0_API/include/ble_gap.h`):

| | |
|---|---|
| `BLE_GAP_ADV_SET_DATA_SIZE_EXTENDED_MAX_SUPPORTED` | **255** |
| `BLE_GAP_ADV_SET_DATA_SIZE_EXTENDED_CONNECTABLE_MAX_SUPPORTED` | **238** |
| `BLE_GAP_SCAN_BUFFER_EXTENDED_MAX` | **1650** |
| adv types | all six `BLE_GAP_ADV_TYPE_EXTENDED_*`, connectable and not |
| PHYs | `primary_phy` / `secondary_phy`, so 2M and Coded are available |

255 against a 248-byte wire, with the envelope inside it. It fits, and the
receive side has room to spare.

The entry point is `sd_ble_gap_adv_set_configure()` — the same call
Bluefruit makes, with an extended type and a bigger buffer.

## What to do about it

**Do to Bluefruit what `tinynimble` did to NimBLE**: skip the vendor's
convenience layer, speak to the thing underneath it, and keep only the
surface XPRS actually uses. That is not a new idea here, it is this project's
established move, and `common/tinynimble/README.md` carries the measurements
that justified it the first time.

The seam already exists and does not need inventing. `xprs_bearer_ble`
publishes `xprsble.h` — `xprsble_start`, `_send`, `_digipeat`, `_set_rx_cb`,
`_is_active` — and already selects between **two** backends behind
`CONFIG_XPRSBLE_BACKEND_TINYNIMBLE`. A third is a third branch in the same
file, not a rewrite:

```
xprsble.h            the bearer's contract, unchanged
  ├── NimBLE         the reference path, kept so the wire can be A/B'd
  ├── tinynimble     HCI straight to the ESP32 controller
  └── (new) SoftDevice   sd_ble_gap_* on the nRF52840
```

**The code does not port; the shape does.** `tinynimble` writes HCI packets
to an ESP32 controller over VHCI. The SoftDevice is not an HCI controller —
it is a linked binary reached through ARM `SVC` calls, with its own event
queue. So a `common/tinysd` (or a `#if` inside `xprsble.c`, if it stays
small) is a new implementation of a known-good contract, and the existing
host tests for the codec and the bearer sit above it untouched.

Roughly what it has to do:

1. Enable the SoftDevice with a `ble_cfg` that asks for an extended
   advertising set, and hand it its RAM start.
2. `sd_ble_gap_adv_set_configure()` with
   `BLE_GAP_ADV_TYPE_EXTENDED_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED` and a
   255-byte buffer; `sd_ble_gap_adv_start()`.
3. `sd_ble_gap_scan_start()` with an extended scan buffer, and read
   `ble_gap_evt_adv_report_t` — which carries `primary_phy`, `secondary_phy`
   and the data pointer.
4. Same receive discipline as everywhere else: the report arrives in the
   SoftDevice's interrupt context, so copy and return. `docs/esp32.md` states
   the rule; it is stricter here for the same reason it is stricter in
   `tinynimble`.

### The constraint that will shape the design

```
BLE_GAP_ADV_SET_COUNT_MAX  (1)
```

**One advertising set on S140. Not a default — the maximum.** An ESP32
controller will run several; this chip runs one, so every advert this station
ever sends time-shares a single slot: the XPRS beacon, and anything
connectable it wants to offer (see `ble5-gatt.md`).

That is not a new problem, only a harder version of one already written down.
`ble5.md` §1 opens with "one radio, one advertising set, and it cannot listen
while it talks", and says a caller with several frames rotates them itself.
The doctrine carries over intact; the nRF52 simply removes the option of
cheating.

## Alternative considered: Zephyr

Zephyr's controller does extended advertising properly (`CONFIG_BT_EXT_ADV`)
and has an SX126x driver besides, so on paper it is the better long-term
home. It is not the recommendation today for two reasons: PlatformIO's board
definition for this module lists `frameworks: ["arduino"]` only, so it is a
toolchain change and not a flag; and the working LoRa station on this board
is Arduino today, so Zephyr means porting something that already runs in
order to add something that does not. Worth revisiting if a second nRF52
board arrives, or if the SoftDevice route runs into a wall.

## Status: built and measured, 2026-08-30

`common/tinynimble/tn_port_sd.c` is the port described above -- the same
`tinynimble.h` surface as the ESP32 port, over `sd_ble_gap_*` instead of
HCI. Measured on the P1-Pro against the bench:

- **Extended advertising, both directions.** The board airs its XPRS beacon
  as a 255-byte-class extended advert and the T-Dongle digipeats it:
  `ble rx -56 dBm 50B t:observation f:X54W6W link:ble peers:0 via:X3WWAJ`.
  It hears the dongle's 123-byte signed observation on BLE and bridges it
  onto LoRa.
- **The mesh channel over a connection**, dialling a T-Deck: see
  `ble5-gatt.md`, "Measured".

Two things the SoftDevice cost that the ESP32 port does not, both now in
the port with a comment on each:

- `NRF_POWER` is a restricted peripheral and TinyUSB holds its interrupt for
  VBUS detection; enable the SoftDevice without releasing it and the answer
  is `4097`, `NRF_ERROR_SDM_INCORRECT_INTERRUPT_CONFIGURATION`, naming no
  interrupt.
- The Arduino core's `enterSerialDfu()` writes `NRF_POWER->GPREGRET`
  directly, so PlatformIO's 1200-baud reflash touch faults the MCU under
  the SoftDevice and the board hangs with a dead USB port -- only RST brings
  it back, and a double-tap into the bootloader is the only way to get a
  fixed image on. The port's fault handler now finishes the reset the core
  meant to do (the register write has already landed by then), and the
  firmware has a `D` console command that enters DFU through the
  SoftDevice properly.

## The chain, end to end

2026-08-30, with the send door's `bearer` parameter (docs/API.md) so the
origin left on LoRa and nothing else:

```
>>HTTP  POST 192.168.178.133/api/xprs/send {"wire":"t:message f:X3GSLC ... m:chain test A","bearer":"lora"}
        -> {"ok":true,"id":"109087","bearers":"lora",...}
CAP     lora rx -42 dBm 66B t:message f:X3GSLC ... m:chain test A                      (SenseCAP, LoRa)
DONGLE  ble      -71 dBm 77B t:message f:X3GSLC ... via:X54W6W m:chain test A           (T-Dongle, BLE5)
LINUX   192.168.178.102 84B t:message f:X3GSLC ... via:X54W6W,X3WWAJ m:chain test A     (UDP 4242 on the host)
```

T-Deck → LoRa → SenseCAP → BLE5 → T-Dongle → WiFi/LAN → Linux, 1.4 s, the
`via:` path naming each hop. Twice.

