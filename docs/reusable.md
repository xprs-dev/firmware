# Reusable code in the XPRS firmware

Pieces written or found to be reused across boards and bearers. Reach for
these before writing a new one; add to the list when you write something with
reuse potential.

## Memory placement (the binding constraint is internal RAM — docs/esp32.md)

- **`xprs_big_calloc(n, size)`** — `common/xprs_common/include/xprs_psram.h`.
  Allocate a large CPU-only buffer PSRAM-first, internal-heap fallback, so it
  is not `.bss`. The allocation twin of the `XPRS_PSRAM_BSS` attribute. Use for
  any buffer big enough to matter on a board whose RF sample bank (xwifi) caps
  how far static data may grow. Host arm is plain `calloc`.
- **`XPRS_PSRAM_BSS`** (same header) — one-word attribute to move a large
  static into external BSS. Inert without PSRAM / off-target, so annotated code
  still builds everywhere. NOT for anything touched by an ISR, by DMA, or while
  flash is being written.
- **Bearer state as a pointer, allocated at `*_start`** — the pattern
  `xprs_bearer_lora` always used and now `xprs_bearer_now/_lan/_ble` too:
  `static xb_t *s_x;` + `if (!s_x) s_x = xprs_big_calloc(1, sizeof *s_x);` in
  start. Every `xb_*` entry is NULL-safe, so a never-started bearer behaves as
  before. Keeps 3.4 KB per bearer out of `.bss` under the bank ceiling.
- **File-scope array → `big_calloc` pointer** — `static T s_x[N];` →
  `static T *s_x;` + `s_x = xprs_big_calloc(N, sizeof *s_x)` at a boot hook.
  Every `s_x[i]` / `&s_x[i]` use site is unchanged (a pointer indexes like the
  array). Check first for `sizeof s_x` (becomes pointer size) and bare `&s_x`;
  `sizeof s_x[0].field` is fine. Fatal-on-fail at boot = no per-use NULL guard.
  Done for the six biggest `libxprs_app` arrays and the four bearer structs.
- **`render_scratch(&slot, bytes)`** — `xprs_app.c`. The lazy PSRAM-first /
  internal allocator for a UI render buffer: first call allocates, later calls
  reuse, NULL on OOM (caller skips the draw, retries next tick). Use for a
  function-local render static so a headless board that never renders pays a
  pointer, not the buffer (`mm`, the chat panel, went this way).
- **Claim contiguous-internal-RAM tasks at the top of `xapp_run()`**, while the
  heap is whole — the event loop + `esp_netif_init()` are done there now, and
  `wifi_up()` tolerates the already-created state. The same rule the big task
  stacks, the echo buffer and httpd already follow. A FreeRTOS task stack
  cannot come from PSRAM, so on an xwifi board (banks reserved) claiming it
  late fails.

## Relay / wire (shared by every bearer — do not reimplement per bearer)

- **`xprs_bearer` (`xb_*`)** — the queue, the §5 dedup rings, the §13.2.1
  cancel, the hop budget, the beacon cadence, the duty ledger. A bearer is just
  a radio + a `xw_port`-style ops table; everything above the complete packet
  is here. `xprs_bearer_now/_lan/_ble/_lora` all wrap it.
- **`xprs_codec` (`xprs_*`)** — the 250-byte text wire, the §5 identifier
  (sig/via removed), `xprs_looks_like`, `xprs_append_via`. One grammar, every
  client (firmware, hotspot page, phone app).

## Time (XPRS.md 4.8, 11.10, 15.7)

- **`xst_clock_set(src, epoch, who)` / `xst_clock_src()`** — `common/xprs_station`.
  THE place a wall clock is set from anything but NTP. The ladder (none < hint
  < owner < ntp): a lower source never overrides, nothing but NTP turns a
  valid clock back (the chip keeps one across a reset: "kept"), out-of-range
  epochs are refused whoever says them, every change is logged with its
  source. `xst_clock_from_ntp()` records SNTP's own
  write. Host-tested (`test_xprs_station_host.sh`). Any new "set the time"
  path (GPS, a future command key) goes through this, never `settimeofday`.
- **`ts_epoch()` / `civil()` / `date_prefix()`** — `common/xprs_meshtastic/mt_mesh.c`.
  `ts:YYYY-MM-DD_HH:MM:SS` ↔ seconds since 1970 (Hinnant's civil algorithm,
  no libc `mktime`/`gmtime` and their locale/TZ surprises), and the "[14:26] "
  / "[10-23 14:26] " / "[14:26Z] " prefix for a text whose carrier cannot say
  when it was written. `xauth_ts_epoch` (xprs_auth) is the same parse for the
  app; worth one shared home when a third user appears. The MeshCore bridge
  (`mc_mesh.c`) has the same gate (no prefix: its frames carry the sender's
  `ts:`) — candidate to lift into a `mesh_common`.
- **A share of the hour per sender** — `sender_spent()` / `sender_note()`
  in `mt_mesh.c`: four timestamps per sender, "spent" when all four are inside
  the last hour. Sixteen bytes, no bucket array; the shape for any per-party
  cap inside a shared budget. Duplicated verbatim in `mc_mesh.c` -- the two
  bridges' gate/share/stale logic is the first thing to lift into a shared
  `mesh_common` when a third network arrives.
- **The mirror gate** — `mt_mesh_on_xprs`: heard traffic crosses to another
  network only under a clock and after `MT_MIRROR_SETTLE_MS`, old news
  (`MT_*_FRESH_S`) is counted and said, not silently dropped. Pattern for any
  bridge that re-airs what it heard.

## Flashing / bench (tools/)

- **`tools/flash-chunked.sh <port> <bin> <offset>`** — writes 256 KB at a time
  with per-chunk retry; the reliable way onto a native-USB S3 (T-Deck/T-Dongle)
  whose single-write flash dies partway, and the only way to recover a
  boot-looping one (the churning USB device fails a plain `pio upload`).

## Screen layout and text

- **`common/xprs_ui/radar_layout.{c,h}`** — pure placement for anything drawn
  around points on a round scope: `rl_spread_dots()` keeps dots apart (and off
  keep-out rectangles) by turning them around the centre without changing their
  radius; `rl_place_labels()` gives each label the longest of up to three forms
  that has a fully clear spot inside the circle, tries three passes so a long
  early label cannot crowd out a short later one, and HIDES a label rather than
  pushing it somewhere wrong. `rl_short_name()` is the Meshtastic short name
  (last four hex digits of an `MT` node). Host test: `test_radar_host.sh`.
  Reuse the shape for any map or scope view; keep LVGL out of the geometry so
  it stays testable.
- **`common/xprs_ui/xui_font_12.c` / `xui_font_14.c`** — Montserrat with
  Latin-1, Latin Extended-A, typographic punctuation and the euro sign, plus
  LVGL's symbol glyphs. LVGL's built-in Montserrat is ASCII only and draws a
  box for "á". Use these for any text that came from a person. Regenerate
  with LVGL's `scripts/built_in_font/built_in_font_gen.py` and lv_font_conv
  1.5.2; the range is in each file's Opts line. The big UI sets 14 on the
  screen object so everything that names no font inherits it.
- **`common/xprs_api/xapi_jesc.c`** — the JSON string escaper: valid UTF-8 is
  copied whole, any byte that is not part of a valid sequence is escaped as
  `\u00XX`, controls and DEL escaped. Host test: `test_jesc_host.sh`. Every
  JSON this firmware writes from text should go through it; two other
  escapers (`msgstore.c`, `ble_hello.c`) pass high bytes raw and would emit
  invalid JSON on a stray byte.
- **`fmt_age()` in `xprs_app.c`** — "12s" / "5m" / "3h" for a narrow column;
  the UI's Traffic table formats the same way inside `xui_flow_rows`.
- **Sizing a table column**: measure, don't guess. The advance widths are in
  the font's `.adv_w` table (1/16 px); a column needs the widest value it
  holds plus `pad_hor` on both sides.

## Answering a station

- **`reply_lanes()` / `reply_air()` in `xprs_app.c`** -- where a directed
  answer goes (XPRS.md 12.0): the bearer the ask arrived on plus the local
  bearer the asker was last heard on, or every local bearer when the asker
  is unknown. Every `t:result` and every replayed record already uses it;
  anything new that answers a particular station (a `cmd:file` grant, a
  future `cmd:who`) should call `reply_air` rather than `idx_air` with the
  arrival bearer, or a relayed ask gets its answer on the relay's radio.
- **`same_base_call(a, b)`** -- callsign equality ignoring a `-n` device
  suffix and case, for comparing an `f:` with a device-table entry.
- **The `cfg` console lines are an interface, not a debug aid.** The app's
  flasher claims a board it just wrote over the same USB cable
  (`app/lib/services/flash/station_console.dart`): it waits for the banner
  `XPRS station <call>`, sends `cfg get own1`, `cfg set own1 <npub>`,
  `cfg set own1c <call>`, `cfg del own2..own4`, and matches the answers
  `own1=<v>`, `<key>=<v> (saved)` and `<key> cleared` exactly as
  `xcfg_console` prints them. Change those words, or the owner keys
  `pol_take_owner` writes, and change the app with them.
- **`xst_dev_path(row, window, &path)` (`xprs_station`)** -- how far a
  device-table row is, in metres, with the relay legs added up: `total_m`
  (-1 while a leg is unknown), `near_m` (to the nearest relay, or the whole
  way for a neighbour), `far_m`, `floor_m` (the known legs: print `>N m`
  with it when the total is unknown), `relay`. The HTTP devices list, the radar
  and the Reachable table all draw from it; anything that prints a distance
  for a station should, rather than `xst_est_distance_m(row.rssi)`, which
  for a relayed row is the relay's distance.
- **`xst_far_note(observer, call, digit)` / `xst_far_rssi()`** -- what the
  neighbours said they hear and how loud (their `hears:` + `zhq:`), one
  (observer, callsign) pair a row. Fed from `seen_note` for every
  observation heard in person or verified. Reuse it for any "how far is X
  from Y" question (a route choice, a map) instead of parsing observations
  again.
- **`via_tail()` and `xst_dev_t.via`** -- a packet's relay chain with this
  station's own callsign left out (a gateway's translation names the
  gateway, XPRS.md 9.11.5), the nearest two kept. `xst_ingest_parsed_ex(...,
  hops_more)` adds hops the wire cannot express (a Meshtastic node's own
  relays, from the frame's hop_start - hop_limit, carried through
  `mt_mesh_ops_t.deliver`'s `rssi, hops`).
- **`heard` hook (`mt_mesh_ops_t`, `mc_mesh_ops_t`, `xprslora_mt_hooks_t`)
  -> `xst_dev_note_hops(call, "lora", rssi, hops)`** -- every decoded frame
  of the other network puts its node in the devices table at its own
  signal, not only the frames that become a translation. A new bridge (or
  any other listener that learns of a station without an XPRS wire) should
  report sightings this way rather than through a fake wire.

## Reticulum beyond single packets

- **`common/xprs_rns/rns_link.{c,h}`** -- an RNS link, initiator side,
  wire-compatible with RNS 1.3.5 and with reticulum-dart (`rns_link.dart`):
  request, proof verification, key derivation, token encrypt and decrypt.
  Anything that needs a link to a peer (a resource, a request/response)
  starts here; never re-implement the handshake.
- **`common/xprs_rns/rns_resource.{c,h}`** -- an RNS resource RECEIVED AS A
  STREAM: parts in a bounded cache, CBC and the three digests running, the
  plaintext handed to a sink callback, so a 1.4 MB image goes into the OTA
  slot through a few kilobytes of RAM. Any large fetch over Reticulum (an
  image, a listing, a card of tiles) is this plus a sink. The part hashmap
  is a RING (`RNS_RES_RING`, 256 slots, `slot_of()` / `in_ring()` /
  `advance()`): a segment's map is never held whole, because only the
  hashes between the frontier and the known horizon are ever consulted,
  and the horizon runs at most two batches ahead. The part cache is
  `RNS_RES_CACHE_PARTS` separate 472-byte blocks taken lazily
  (`cache_slot()`), because on the C3 one 6.8 KB calloc failed with 16 KB
  free: a fragmented heap gives small blocks when it will not give one
  large one, and a slot it refuses only narrows the window. Two rules for
  any receiver on a board without PSRAM: keep the window, not the table;
  and ask the heap for many small blocks, each one optional.
- **`rns_token_encrypt/decrypt`, `rns_ed25519_verify`, the incremental
  `rns_sha256_*`, `rns_hmac_*`, `rns_cbc_decrypt_*`** in `rns.c`: the
  primitives, with host (OpenSSL) and device (mbedtls) bodies behind one
  interface, as the rest of that file already did.
- **`test_rns_link_host.sh`** -- the pattern for testing C against the Dart
  implementation without a radio: `reticulum-dart/tool/files_responder.dart`
  is the real `xprs/files` responder speaking hex lines over pipes.

## Folders and firmware

- **`common/xprs_folder`** -- the XFL1 listing walker, folder and member
  description readers, `<board>-<version>.bin` parsing and a dotted version
  compare (`xfolder_version_cmp`). Reuse the version compare wherever a
  version string is ordered; it is numeric, not lexical.
- **Tables allocated at init, read from other tasks (`chat_ensure()`,
  `peer_find()`, `xst_stats_load()`)** -- on the single-core C3 the indexer
  and the bearers are up before `app_main` reaches `xst_init()` and
  `xprsrns_init()`, and a packet heard two seconds into the boot reaches
  their tables first. Two shapes, both already in the tree: allocate on
  first touch (`chat_ensure()`, which the stats loader now calls too), or
  make every reader answer "nothing yet" while the pointer is NULL
  (`s_peers` in xprsrns.c). A new `xprs_big_calloc`'d table takes one of
  the two; a bare `s_table[i]` from another task is the C3's boot panic.
- **`common/xprs_fwfetch` (`xfw.c`)** -- a state machine with every outside
  effect injected (sign, send, route, sink, clock, lock), the same shape the
  Dart core uses for routing. Copy the shape for the next unattended duty.
- **`xota_push_begin_sha()`** -- an install approved by a digest the caller
  already trusts (a verified signed description), beside the `xprsfw1`
  approval line. `xota_publisher_key()` exposes the pinned `fwkey` to
  whoever verifies the publisher's packets.
- **`reply_lanes()` / `reply_air()`** -- see "Answering a station" above.
- **`fw_file_digest()` / `fw_prune()` (xprs_app.c)** -- hash a file on the
  card in 2 KB bites with a breath every 64 KB, and drop every file of a
  `<board>-` family but one. The pattern for anything that keeps a
  content-addressed copy on /idx: land it as `.part`, prove it, rename it.
- **`tools/xsign.c`** -- sign and `--verify` one packet line with the
  station's own signer, host-built. `tools/scripts/publish_folder.py`
  wraps it with `bech32_decode()` / `bech32_encode()` (nsec, npub) and the
  packet signing rule (sig over the line without `sig:`/`via:`, placed
  before `m:`); reuse those three for any tool that signs packets outside
  the app.

