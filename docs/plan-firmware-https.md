# Plan: HTTPS and an xprs.dev name on the boards

Status 2026-11-10: **a plan, not started.** It resumes steps 3, 5, 6 and 7 of
`plan-esp32-home-station.md` with what exists since. The goal: a board on
the internet answers as `https://<callsign>.xprs.dev`, with its own
certificate, like the Linux server station.

## What is already done

- Reticulum by default on the core's hubs: a board with internet is
  reachable over Reticulum today (outbound uplink, no open port needed).
- The visitor API is internet-safe in three places it was not: `POST
  /api/wifi` only from the administrator while online, the admin claim only
  from the station's own network, mailbox declarations and receipts checked
  against their key.
- Long file downloads leave the single httpd worker (async task); files stream
  with Range.

## What the server does, and what the board lacks

The server's path (core `public_name/`, `names/`):

1. Learn its public addresses (IPv6 from the interface, IPv4 as the keeper sees it).
2. GET a challenge from `names.xprs.dev`, POST a signed `t:service` claim
   (`f:`, `k:` npub, `dial:` the address the request leaves from, `m:` the
   challenge). The keeper checks the signature against `k:`, that `dial:` is
   the request's source, and pings `t:ping` on TCP 4242 at that address
   before it writes the `AAAA`/`A` record through deSEC.
3. Get a Let's Encrypt certificate by HTTP-01 on port 80, serve HTTPS on 443,
   redirect 80 to 443, renew 30 days before expiry.

The board has none of these: no TCP 4242 listener, no names client, no ACME
client, no HTTPS server, IPv6 compiled out on every board without PSRAM.

## Decisions (proposed answers in bold)

1. **Which boards.** TLS costs 25-45 KB of heap per connection.
   **T-Deck first** (8 MB PSRAM: mbedTLS allocates there). T-Dongle second
   (no PSRAM, about 100 KB free after boot: one TLS connection at a time, the
   ACME run at a quiet moment). The C3 stays plain HTTP on its own network and
   is reached from the internet over Reticulum only.
2. **The house's one public IPv4** belongs to the Pi (80/443/4242). Options:
   (a) **boards on IPv6 only**: each has its own global address and its own
   FRITZ!Box IPv6 pinholes (80, 443, 4242); the name gets an `AAAA` record
   only. IPv4-only visitors cannot reach them. (b) as (a), plus the Pi
   forwards `<board>.xprs.dev` on IPv4 by SNI without decrypting (TLS still
   ends on the board, with the board's certificate). **(a) now, (b) later.**
3. **How the certificate is proved.** **HTTP-01 on port 80, as the server
   does** (Let's Encrypt validates over IPv6 when the name has only `AAAA`).
   The alternative, DNS-01 written by the keeper on the station's signed
   request, needs no port 80 but a new keeper endpoint; kept as a fallback for
   stations that cannot open 80.

## Steps

1. **IPv6 on the T-Deck and the T-Dongle.** `CONFIG_LWIP_IPV6=y`, SLAAC, a
   stable global address (no privacy addresses: the name must keep pointing
   at it), the address in `/api/status`. Measure the heap cost on the dongle
   (it was turned off there for RAM in footprint.md).
2. **Hardening for the open internet** (before any pinhole):
   - every endpoint classed local or internet (`xapi_wc_local()`); the config
     share, `/api/diag`, coredump, OTA push and the console LAN-only;
   - per-address rate limits on all writes and on file uploads, a cap on
     concurrent sockets per address, the httpd LRU purge on;
   - a slow-client limit (recv/send timeouts already short) and refusal of
     oversized headers early.
3. **TCP 4242, XPRS text lane** (step 3 of the house plan, reduced to what
   the keeper needs): a listener on IPv6 (and IPv4), one task, a few clients;
   answer a signed `t:ping` with a signed `t:pong`; first bytes sniffed so a
   Reticulum HDLC client gets a polite close (a Reticulum server side is a
   later step; the outbound uplink already makes the board reachable there).
4. **The names client in C** (`common/xprs_names`), a port of
   `names_client_io.dart`: HTTPS client to `names.xprs.dev` (esp_http_client,
   the certificate bundle), challenge, claim signed with the station key
   (`xprs_sig`, the same 48-byte signature the keeper verifies), sent from the
   global IPv6 address it names; claim again on address change and before the
   record's lifetime ends; state in NVS; the name and its state in
   `/api/services` (`urls`, as the server reports them).
5. **ACME client in C** (`common/xprs_acme`): an ES256 account key in NVS,
   JWS with mbedTLS ECDSA, new-order, the HTTP-01 token served by the plain
   httpd at `/.well-known/acme-challenge/<token>`, finalize with a CSR
   (`mbedtls_x509write_csr`, P-256 key), the chain and key stored on the
   flash volume (`/idx/tls/`), renewal at 30 days left, backoff on failure,
   Let's Encrypt staging first as the reachability test (the server's trick),
   production once staging succeeds. Host tests for JWS, CSR and the state
   machine against recorded answers.
6. **HTTPS server** with `esp_https_server` on 443, sharing the URI handlers
   of the port-80 server (one table registered on both); mbedTLS allocating
   from PSRAM on the T-Deck (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`); at most 2-3
   TLS sessions; HTTP/1.1 keep-alive. Port 80 keeps serving the station's own
   network (phones on the hotspot speak plain HTTP to an IP) and redirects
   internet visitors to `https://<name>/`.
7. **The page and the API know the name.** `/api/services` `urls` and the
   Station view show `https://<name>`, as on the server; the page already
   reads them.
8. **Tests without the hardware.**
   - QEMU (`tools/qemu_c3.sh`, OpenCores Ethernet) against a **Pebble** ACME
     test server and a **test names keeper** (the Dart keeper with a fake DNS
     writer, `names_keeper.dart`) both on the Pi: the whole claim → ping →
     record → certificate → HTTPS path, scripted like
     `tools/qemu_collections_test.py`. The QEMU build has no IPv6 router:
     test over IPv4 on the emulated LAN, the address check relaxed only in
     the test keeper.
   - The portal's conformance suite run over `https://` against the emulator.
9. **On the T-Deck, at home.** Pinholes on the FRITZ!Box for the board's IPv6
   address (80, 443, 4242), a real claim at `names.xprs.dev`, staging then
   production certificate, a week's soak with heap and handshake counters.
10. **Later:** the T-Dongle (one TLS session), the Pi's SNI forwarding for
    IPv4 visitors (decision 2b), a Reticulum server on 4242, DNS-01 through
    the keeper (decision 3).

## Risks

- Heap: TLS plus the station on one chip; on the T-Deck it is PSRAM's job, on
  the dongle it decides whether HTTPS fits at all.
- The single httpd worker: a slow TLS client holds it longer than a plain one
  (the handshake alone is ~0.5-1 s of CPU on an S3). The async hand-over
  covers downloads only.
- A stable IPv6 address: if the prefix changes (the provider renumbers), the
  claim must follow before visitors notice; the keeper allows a new claim from
  the new address with the same key.
- Testing ACME and the keeper end to end without a public address: Pebble and
  the test keeper are the substitutes, and the first real run is at home.
