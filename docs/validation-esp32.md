# ESP32 inter-device validation

The full set of tests between two ESP32 XPRS full stations on the bench, so a
regression shows up against a known-good run. ESP-NOW is the WiFi-class local
bearer (xwifi is not shipped; see xwifi-implementation.md for why it does not
fit the C3).

## The two stations (2026-10-23)

| | callsign | board | IP | bearers |
|---|---|---|---|---|
| A | X3SQS6 | esp32c3-mini | 192.168.1.230 | BLE5, ESP-NOW, LAN, Reticulum |
| B | X3SEP2 | tdeck (ESP32-S3) | 192.168.1.231 | BLE5, ESP-NOW, LAN, LoRa, Reticulum |

Both on the same access point (Vodafone-16552C): ESP-NOW rides the station's
channel, so associating to one AP puts both on one channel without guessing,
and the LAN bearer needs the shared subnet. Both dial rns.wisco.network:4242.

How each test is run is below so it can be repeated; the method is HTTP to each
station's own API (`/api/...`), no serial needed once they are on the network.

## Results

### 1. Mutual discovery, per bearer

`GET /api/xprs/history?from=<peer>&limit=200`, tally the `bearer` field (each
record carries the lane it was heard on). Both heard the other directly
(0 hops) on every shared bearer:

| receiver → sender | espnow | ble | lan | rns | lora |
|---|---|---|---|---|---|
| A hears B (X3SEP2) | ✓ 98 | ✓ 97 | ✓ 3 | ✓ 2 | — (A has no LoRa) |
| B hears A (X3SQS6) | ✓ 30 | ✓ 107 | ✓ 2 | ✓ 1 | ✓ 60 (relayed) |

Direct BLE RSSI A↔B −17 / −24 dBm. `/api/xprs/devices` dedups to the strongest
lane (BLE here); the per-record tally above is how the other lanes are seen.

### 2. Message delivery, both directions

`POST /api/xprs/send` with a unique token, then `GET /api/xprs/history` on the
other station. The station airs on every local bearer (`bearers:lan,espnow,ble`
in the response) and the receiver's dedup collapses the copies to the first to
land.

| direction | sent on | arrived via | result |
|---|---|---|---|
| A → B `t:message scope:local` | lan,espnow,ble | lan | ✓ |
| B → A `t:message scope:local` | lan,espnow,ble | espnow | ✓ |
| A → B `t:message d:X3SEP2` (directed 1:1) | lan,espnow,ble | espnow | ✓ |

### 3. Reticulum peer-to-peer through the public hub

`/api/xprs/peers` → `rns`: `up:true hub:true peers:1`, and
`addressed:{tx:1, rx:1, via_hub:1, path_req:1}` — the station learned the peer
from a path request and exchanged an addressed wire through the hub, not over
the LAN. The rns rows in the discovery tally are the same path.

### 4. Reachability under load

One ping/s while all bearers carry traffic (BLE beacons, ESP-NOW, LoRa/Mesh
bridging on B, the hub link):

| station | loss |
|---|---|
| A (C3) | 0 % |
| B (T-Deck) | ~10 % — it is the busier board (LoRa + Meshtastic bridge + BLE) |

## Verdict

ESP-NOW, LAN, BLE5 and Reticulum all carry XPRS between the two ESP32 stations,
in both directions, with the cross-bearer bridge and the §5 dedup working and
directed 1:1 delivery confirmed. The C3 carries the fleet-wide `.bss`
relocation of 2026-10-23 (static RAM 93,344 → 74,264) and ran clean throughout.
