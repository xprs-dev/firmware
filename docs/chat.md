# The hotspot chat page

The board runs an open access point and serves one page on it. A visitor
joins the AP, the captive portal opens the page, and they can talk to the
mesh without installing anything and without an account.

The page is shared with the Android app and lives in its own repo, `webchat`
(checked out next to this one): `page/chat_page.html`, `page/xprs_crypto.js`.
`common/xprs_hotspot/tool/embed_page.py` asks webchat's `tool/build.py` for the
gzipped page as `chat_page.c`; edit there, run the tool, rebuild. It is served
straight out of flash with `Content-Encoding: gzip` (about 61 KB of page, 25 KB
in flash). The page asks `/api/services` and adapts; the boards answer with no
`page` key, so they show every tab, as before.

`common/xprs_http/` is an older, larger implementation of the same idea --
WebSocket, nostr-tools, its own captive URIs. Nothing calls
`http_server_start()`; it is not built into any image.

## What the page shows

Five tabs, taking the same material the board puts on its own screen:

| Tab | Content | Endpoint |
|---|---|---|
| Chat | rooms, messages, replies, reactions | `GET /api/xprs/history`, `POST /api/xprs/send` |
| Traffic | every packet filed, with bearer, RSSI and signature state | `GET /api/xprs/history?limit=60` |
| Devices | who is in reach: link, signal, estimated distance, age | `GET /api/xprs/devices` |
| Stats | devices heard, packets in, packets out | `GET /api/stats?view=0\|1\|2` |
| Station | callsign, uptime, memory, battery, address, LoRa airtime, roles, screenshot | `GET /api/status`, `/api/services`, `/api/screen` |

Only the visible tab polls: chat and traffic every 3-5 s, devices 5 s,
station 10 s, stats 60 s. The station has one HTTP worker and one 2 KB
buffer for every reply, and four clients may be connected.

## Endpoints

`spec/API-HTTP.md` documents the interface. Two exist for the panels above
and are not yet in that document:

- `GET /api/xprs/devices` -> `{ok, heard, count, devices:[{call, bearer,
  rssi, dist_m, hops, age_s, via?, near_m?, far_m?}]}` -- the list
  `xst_devices()` keeps, the same one the Reachable panel prints and the
  radar plots, 300-second window. One row per station: `bearer` and
  `age_s` are the station's latest contact; `hops` is the SHORTEST fresh
  path to it (a neighbour whose packet also came round through a relay is
  still a neighbour), and `rssi` the strongest radio reading on a row of
  that hop count (a LAN packet has no signal and must not cost the row its
  distance; a relayed copy's signal is the relay's and never stands in for
  a direct reading). `dist_m` is the whole way, through
  `xst_est_distance_m()` (RSSI through a log-distance model, an estimate
  and no better than the room): for a neighbour that is its own signal; for
  a station a relay carried here it is the leg to the nearest relay plus
  what that relay itself measured to the station, said in the relay's
  observation (`hears:` with the `zhq:` digits) and kept by
  `xst_far_note()`. While a leg is unknown `dist_m` is -1. A relayed row
  also carries `via` (the nearest relay), `near_m` (the leg to it) and
  `far_m` (the legs beyond, -1 unknown) and `floor_m` (the legs that ARE
  known, added up), so the page can show a floor (`>~12 m via X3SEP2`). A
  node behind a Meshtastic or MeshCore repeater keeps the signal the radio
  measured, which is the repeater's: its `floor_m` is the distance to that
  repeater, and `via` names the repeater when the frame's relay byte (or
  MeshCore path hash) matches exactly one node the bridge knows. On the gateway itself a Meshtastic or
  MeshCore node is a neighbour on `lora` at its frame's signal, from every
  decoded frame and not only the texts that become translations (the
  bridges' `heard` hook; `via:` naming the station is not a hop); a frame
  that came through that network's own relays counts them in `hops` and
  has no distance. Bench 2026-10-26: Meshtastic nodes
  the T-Deck bridged read, on the C3's page, as being where the T-Deck was.
- `GET /api/stats?view=` -> `{ok, view, bucket_s, points, devices[], rx[],
  tx[]}` -- the three series of the Stats panel. `view` 0 ten-minute,
  1 hourly, 2 daily. `points` is 0 until NTP has spoken.

Both are board-supplied bodies (`devices_json`, `stats_json` in
`xprs_api_cfg_t`), like `status_json` and `peers_json` before them.

## Keys and signing

The browser holds the key and does the signing; the firmware never sees a
private key and does not sign for the visitor.

- A key is generated on first load and kept in `localStorage` under
  `xprs_nsec`, falling back to a cookie and then to memory -- captive
  WebViews block storage, and a visitor with no key can still talk.
- The callsign is derived from the public key as section 3 says.
- `xprs_crypto.js` implements the short Schnorr of XPRS.md 9.1.2 in BigInt
  and base85, checked against the worked example by `tool/test_crypto.sh`.
- The page composes the wire, signs it, and posts it; `xapi_send.c`
  validates length, leading `t:` and `f:` and airs it on every bearer.
  A packet over 250 bytes is refused by both ends.

## Where messages live

Nothing is kept in a chat ring. Every packet the station hears or sends goes
to the index (`xprs_index`, on the SD card or flash), and the page reads it
back through `/api/xprs/history` -- so what the visitor sees is what the
station actually filed, dedupe, signature verdict and all. A packet is
identified by its section 5 derived id, not by an arrival number.

The page keeps only view state in the browser: which room is selected, the
`seen` set that stops a re-poll re-rendering a message, and the "cleared
before" mark behind **Clear chat view** (local only -- it hides nothing on
the station and deletes nothing).

## Rooms

`Local`, `Regional`, `Global` and `Social` plus one room per callsign heard,
matching the rail on the board's own Chat panel. A row lands in a room by its
`scope:` and `d:` fields (XPRS.md 9.11), not by anything the page invents,
and a row the station marks `"out":true` (heard on a bearer wider than its
scope) lands in none.

## Logging

The station's log is the rotating file pair `/idx/log/cur.txt` and
`prev.txt`, readable at `GET /api/log?limit=` (default 50, max 500). The
page does not write to it: a visitor's browser is not a source of log lines.

## Files

Files are XPRS section 6.7 -- content-addressed, described by `file:` and
fetched with `cmd:file`. The page does not offer attachments; a 250-byte
packet does not carry one, and the station is not a file server unless it
announces `serve:files`.
