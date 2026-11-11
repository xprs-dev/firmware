# Station profiles

A station runs one of two profiles, set by `profile` (NVS, `cfg set profile
longrange`, or `[station] profile = longrange` in config.ini). It is read at
start: a change takes a restart. The profile is the only switch; what it
implies is below, and the individual settings still apply where they are named.

## Default (`profile = default`)

The station everyone has unless they choose otherwise. It serves its local
devices and the internet well.

- **ESP-NOW** on the WiFi channel, at 1 Mbps. On a router: the router's
  channel, never left. With no router: `espnow_ch`, the board's fallback.
  Its WiFi stays connected and stable, so it answers a browser over
  Reticulum without its uplink dropping (docs/espnow.md has the measurement).
- **Every function**: the visitor API over Reticulum (a browser at
  `https://xprs.dev/in/<callsign>`), hosted files, gallery, music and library
  where the board has storage, chat, traffic, administration.

## Long range (`profile = longrange`)

A repeater and archiver that bridges long-range ESP-NOW links and the local
devices, for a site where range matters more than serving visitors.

- **ESP-NOW** on its own channel (`espnow_ch`, 13) with the long-range PHY
  (250 kbps, up to 6 dB more link budget). On a router it visits that channel
  80 ms a second (`espnow_win`); expect the router to drop it now and then
  (docs/espnow.md).
- **Kept:** digipeating and bridging between bearers, the archive and history,
  the chat for local devices, traffic, devices, stats, administration.
- **Off:** the visitor API over Reticulum (its links are not answered; the
  station still carries XPRS wires on Reticulum), and hosted files: no
  uploads (`POST /api/xprs/file` answers 403), so no gallery, music or
  library. Files it already holds are still served.

Long-range and default stations do not meet over ESP-NOW (different channel
and PHY); they meet through the bearers they share (BLE, LAN, Reticulum).
