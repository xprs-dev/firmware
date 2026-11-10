# Plan: HTTPS and an xprs.dev name on the boards

Status 2026-11-10: **a plan, not started.** The goal: a board installed at a
place with its own public IP address answers as `https://<callsign>.xprs.dev`,
with its own certificate, exactly as the Linux server station does. Each
station is independent: it learns its own address, opens its own router,
claims its own name and keeps its own certificate. Nothing depends on any
other station or on IPv6.

## What the server does (the reference, core `public_name/`, `names/`)

1. Learns its public IPv4 address (from the keeper's challenge, which reports
   the address it sees) and its router (UPnP IGD: maps 80, 443 and 4242).
2. GETs a challenge from `names.xprs.dev`, POSTs a signed `t:service` claim:
   `f:`, `k:` (npub), `dial:` (the public address), `m:` (the challenge). The
   keeper checks the signature against `k:`, that the request comes from
   `dial:`, and pings `t:ping` on TCP 4242 there before it writes the record.
3. Gets a Let's Encrypt certificate by HTTP-01 on port 80 (staging first, as the
   test of reachability), serves HTTPS on 443, redirects 80 to 443 for internet
   visitors, renews 30 days before expiry.

## What the board lacks

No TCP 4242 listener, no UPnP client, no names client, no ACME client, no
HTTPS server. The visitor API is already internet-safe in the places that
mattered (WiFi changes, admin claim, mailbox declarations); files stream and
long downloads leave the httpd worker.

## Decisions

1. **Which boards.** A TLS session peaks at ~12 KB with dynamic
   buffers (measured below). **The T-Deck and boards with PSRAM** first, then
   the T-Dongle (no PSRAM, ~100 KB free: one TLS connection at a time), then
   the C3 in an internet profile with BLE off (measured below).
2. **The TLS itself: mbedTLS, through `esp_https_server`** (in ESP-IDF
   5.2.1). A TLS stack is not something to write again; what is ours is the
   glue: certificates from our ACME client, one URI table on both ports, the
   memory limits. If `esp_https_server`'s one-server-per-port design costs too
   much (a second httpd task and its sockets), the fallback is our own small
   TLS front: an mbedTLS session per accepted socket on 443 that hands the
   decrypted bytes to the existing httpd over a loopback socket.

## The C3, measured (2026-11-10)

Env `c3https` (models/esp32c3-mini, never published): the station plus
`esp_https_server` on 443 with a self-signed ECDSA P-256 certificate, mbedTLS
dynamic buffers (in 16 KB, out 4 KB), session tickets; `/heap` samples the
internal heap every 10 ms. Bench C3 X3VGAZ on the house router, Reticulum on,
hotspot on, the real 88 KB chat page served over HTTPS.

| configuration | free after boot | HTTPS |
|---|---|---|
| everything on (BLE, hotspot, Reticulum) | 22.5 KB, largest 9 KB | **does not start** (no 10 KB block for its task) |
| BLE off | 64 KB, largest 47 KB | starts; 54 KB free with it idle (5 KB task stack) |

With BLE off:

| load | result |
|---|---|
| one connection: handshake + page | 0.41 s TLS, 1.2 s for the 88 KB page; ~12 KB per session at its peak |
| 10 new connections in a row | 0.36-0.48 s each, memory back each time |
| keep-alive, 5 pages on one connection | 0.47 s in all |
| a real browser (headless Chromium), 3 loads | the whole page in 2.7-3.0 s |
| 4 at once, one socket, the others queued | all served, 0.9 s apart; low point 21 KB free, largest 5.6 KB |
| 2 sockets, LRU purge on, 4 at once | some dropped; then a **panic in lwIP's task** (heap at 9.8 KB) |
| 8 at once, one socket, queued | all failed and the **network stack wedged** (no ping, no HTTP, no reset) until reboot |

The HTTPS task used about 2.5 KB of stack (5 KB is enough). The code costs
~96 KB of flash: the C3 image goes from 88 % to 94 % of its slot.

**Verdict.** The C3 can serve HTTPS, at one visitor at a time, only with BLE
off, and only once connection floods can no longer wedge it: queued
connections hold lwIP memory while they wait. Before a C3 is exposed: a
backlog of 2, new connections refused (closed at once) when the free heap is
under ~30 KB, a cap per address, smaller TCP windows for the TLS socket, and a
watchdog that restarts networking when nothing has been received for a while.
So the C3 takes an "internet profile" (BLE off, these limits); the T-Deck
stays the first target, and the C3 profile comes after the same limits are
proven there.

## Steps

1. **Hardening for the open internet** (before any port is opened):
   - every endpoint classed local or internet (`xapi_wc_local()`): the config
     share, `/api/diag`, coredump, OTA push and the console LAN-only;
   - per-address rate limits on all writes and uploads, a cap on concurrent
     sockets per address, the httpd LRU purge on, short header limits.
2. **TCP 4242, XPRS text lane**: a listener with a few clients; a signed
   `t:ping` answered with a signed `t:pong` (what the keeper tests); the first
   bytes sniffed so a Reticulum client gets a clean close (a Reticulum server
   side is a later step: the outbound uplink already reaches the board).
3. **UPnP IGD client in C** (port of `upnp_io.dart`): SSDP discovery, the
   gateway's control URL, `AddPortMapping` for 80, 443 and 4242 to the board's
   LAN address, renewed before the lease ends, removed on shutdown. A router
   that refuses (403, no UPnP) is reported on the Station view with the three
   forwards to make by hand.
4. **Names client in C** (`common/xprs_names`, port of `names_client_io.dart`):
   HTTPS client to `names.xprs.dev` (esp_http_client, the certificate
   bundle), the challenge (which also gives the public IPv4), the claim signed
   with the station key (`xprs_sig`, the signature the keeper verifies), a new
   claim when the public address changes and before the record's lifetime
   ends, state in NVS, the name in `/api/services` `urls` as the server reports it.
5. **ACME client in C** (`common/xprs_acme`): an ES256 account key in NVS,
   JWS with mbedTLS ECDSA, new-order, the HTTP-01 token served by the port-80
   httpd at `/.well-known/acme-challenge/<token>`, finalize with a P-256 CSR
   (`mbedtls_x509write_csr`), chain and key on the flash volume (`/idx/tls/`),
   staging first, then production, renewal at 30 days left, backoff on failure.
   Host tests for the JWS, the CSR and the state machine against recorded answers.
6. **HTTPS on 443** (decision 2), the same URI handlers as port 80, mbedTLS in
   PSRAM, 2-3 sessions at most, keep-alive. Port 80 keeps serving the station's
   own network (phones on its hotspot speak plain HTTP to an IP) and redirects
   internet visitors to `https://<name>/`.
7. **The page shows it**: the Station view's addresses and name come from
   `/api/services` `urls`, as on the server (the page already reads them).
8. **Tests without hardware**: QEMU (OpenCores Ethernet, IPv4) against a
   **Pebble** ACME test server and a **test names keeper** (the Dart keeper
   with a fake DNS writer and a UPnP stub), both on the Pi: claim, ping, record,
   certificate, HTTPS, scripted like `tools/qemu_collections_test.py`; the
   portal's conformance suite over `https://`.
9. **On hardware**: a T-Deck at a place where it owns the public IP (or with
   the router's forwards pointed at it for the test), a real claim, staging
   then production, a week's soak with heap and handshake counters.
10. **Later**: the T-Dongle with one TLS session; IPv6 where a place has it
    (an `AAAA` beside the `A`); a Reticulum server on 4242.

## Risks

- Heap: TLS beside the station; PSRAM carries it on the T-Deck, the dongle
  decides whether HTTPS fits without it.
- The single httpd worker: a TLS handshake is 0.5-1 s of CPU on an S3; two
  visitors arriving together wait on each other. Long downloads already leave it.
- Routers without UPnP: the station cannot open itself; the Station view
  says which forwards to make.
- A changing public IP (dynamic addresses): the names client must reclaim
  quickly; the record's lifetime and the keeper's rules already allow it.
