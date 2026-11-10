# Plan: the ESP32 as the house's always-on station

Status 2026-11-09: **a plan for later, nothing implemented.** The goal: an ESP32
station that does what the Linux server station does, reachable from the
internet and Reticulum with the same functions, that claims its own
`<callsign>.xprs.dev` from the names keeper and replaces the Raspberry Pi as the
station that is always on in the house.

## Where the firmware stands

Already there (shared station code, `common/xprs_app`): the web chat page and
its API (`/api/xprs/send`, `history`, `mail`, `dir`, `services`, `session`,
`admin`, `rooms`, passing the webchat conformance suite); the archive with
eviction, mailbox declarations, visitor mailboxes, gossip, `cmd:history`, the
directory; the secp256k1 identity and X3 callsign; signed owner commands; signed
OTA with rollback; firmware fetch over Reticulum.

Missing:

| | Linux station | ESP32 today |
|---|---|---|
| HTTPS / Let's Encrypt | yes | no TLS linked (removed for heap) |
| `<callsign>.xprs.dev`, public address, UPnP | yes | absent; IPv6 off on most boards |
| Reticulum | transport node, TCP 4242 server, several uplinks | client leaf: one uplink, no listener, no routing |
| XPRS text on TCP 4242 (the keeper's ping) | yes | absent |
| Safe on the internet | yes | no: `/api/wifi` and the config editor are open by LAN trust |
| Always-on archive depth | yes | refused on every board (needs 64 MiB: an SD card) |
| Ethernet | n/a | QEMU driver only |

## Decisions to take before starting

1. **Board.** TLS, ACME, several Reticulum peers and HTTPS visitors at once need
   PSRAM. The T-Dongle (no PSRAM) is out. Candidates: the T-Deck (8 MB PSRAM, SD),
   or a dedicated ESP32-S3 board with 8 MB PSRAM, an SD slot and wired Ethernet
   (W5500 class; needs a driver and a `models/` entry). Wired is steadier.
2. **The house's single public IPv4.** 80/443/4242 forward to one device only;
   the Pi also serves `names.xprs.dev`, which every claim depends on. Options:
   (a) move the keeper elsewhere; (b) keeper IPv6-only; (c) the ESP32 takes IPv4
   and the Pi sits behind per-port forwards; (d) the ESP32 is the default station
   on IPv6 while the Pi keeps IPv4 until (a). Proposed: (d) now, (a) later.
3. **What "the same functions" means.** The server edition's feature list as the
   contract, excluding the names-keeper role (deSEC token; one keeper suffices)
   and phone-only subsystems (I2P, torrents, folders).

## Steps

0. **Parity suite.** One black-box test against any station URL: the web chat
   conformance suite for `/api/...`, a signed `t:ping` -> `t:pong` on 4242, a
   Reticulum handshake from a stock `TCPClientInterface`, a names claim against a
   test keeper. Run it on the Pi first (reference), then on the ESP32.
1. **Bring-up for always-on.** PSRAM for the big buffers, an SD archive of at
   least 64 MiB so `idx_is_always_on()` accepts it, IPv6 on, Ethernet if the
   board has it. Fix the two known wedges first: `idx_task` stuck on `rns_tcp`
   DNS while WiFi flaps, SD-backed endpoints timing out on long runs. A
   multi-day soak with heap and watchdog counters.
2. **Safe to expose.** Every endpoint classified local or internet
   (`xapi_wc_local()` already tells them apart); `/api/wifi`, `/config`, admin
   LAN-only or a signed owner command; connection caps, read timeouts, per-request
   heap limits, rate limits. Before any port forward exists.
3. **TCP 4242, XPRS text side.** A listener (IPv4 and IPv6), several clients,
   first bytes sniffed per connection (XPRS lines or Reticulum HDLC, as the Linux
   port does). Port the line lane of `xprs_tcp.dart`: signed `t:pong` to `t:ping`,
   `q:mail`, identity asks for unknown keys, held mail delivered on the socket.
4. **Reticulum as a node.** Several hub uplinks, accepted clients as interfaces,
   announce validation and rebroadcast, a bounded path table, forwarding, a
   minimal link responder, the `dial:<callsign>.xprs.dev` announce once the
   keeper's ping succeeds. Memory bounds explicit; the Dart `RnsTransport` is the
   reference.
5. **Public addresses and router.** Global IPv6 from SLAAC, public IPv4 as the
   keeper's challenge reports it, nip.io names as fallback, a UPnP IGD client as
   best effort. The house FRITZ!Box refuses UPnP (403 in the Pi's log), so manual
   forwards here.
6. **The names claim.** Port `names_client_io.dart`: challenge, claim signed with
   the station key and sent from the address it names, the keeper's ping, renewal,
   state in NVS. Check early that the firmware's 48-byte Schnorr is what the
   keeper verifies.
7. **HTTPS.** mbedTLS in PSRAM, HTTPS on 443, port 80 for HTTP-01 and the redirect,
   an ACME client (ES256 account, order, CSR, chain, renewal 30 days before
   expiry), the Let's Encrypt roots for outbound TLS. Rather than staging orders
   as an outside reachability tester, add an HTTP/HTTPS probe to the keeper's
   claim. Test against Pebble and a test keeper in QEMU (its Ethernet works)
   before hardware.
8. **Feature sweep.** Run the step-0 suite, fill what differs (public name and
   addresses in `/api/services` and the Station view, a `cmd:file` responder over
   Reticulum, whatever else shows), keep the embedded page synced.
9. **Updates and operations.** Keep the signed fetch over Reticulum; with TLS,
   also follow GitHub stable releases (`xprs_updates` has the check); health
   counters and a remote log for an unattended box.
10. **Cut-over.** Router forwards per decision 2, the ESP32 claims its own name,
    the Pi stays as keeper and second archiver or retires. A week's soak, then
    `FIRMWARE.md`, `docs/esp32.md` and the xprs.dev/server page gain an "ESP32
    station" option.

## Order and risk

0 -> 1 -> 2 come first. 3 and 6 give the name early: the keeper needs only 4242,
so the ESP32 is reachable over `http://` and Reticulum before step 7 adds HTTPS.
Biggest risks: heap (TLS plus several sockets on one chip), robustness against
hostile traffic, and the two existing wedges, which is why step 1's soak and
step 2's limits come before anything is exposed.
