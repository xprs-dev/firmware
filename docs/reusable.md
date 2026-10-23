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
  `xprs_bearer_lora` always used and now `xprs_bearer_now/_lan/_ble/_wifi8` too:
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
- **Application-provided demod scratch (`xw_scratch_t`)** — `xwifi.h`. The
  xwifi demodulator's working arrays (~6.4 KB) are no longer function statics;
  the application sets `demod.scratch`, placing them where its chip can afford,
  the same contract as the station's `extra_window`. A demod with no scratch
  decodes nothing (entry points return 0).

## Boot order is the allocator (docs/esp32.md)

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
  is here. `xprs_bearer_now/_lan/_ble/_wifi8/_lora` all wrap it.
- **`xprs_codec` (`xprs_*`)** — the 250-byte text wire, the §5 identifier
  (sig/via removed), `xprs_looks_like`, `xprs_append_via`. One grammar, every
  client (firmware, hotspot page, phone app).

## Time (XPRS.md 4.8, 11.10, 15.7)

- **`xst_clock_set(src, epoch, who)` / `xst_clock_src()`** — `common/xprs_station`.
  THE place a wall clock is set from anything but NTP. The ladder (none < hint
  < owner < ntp): a lower source never overrides, an equal one only moves the
  clock forward, out-of-range epochs are refused whoever says them, every
  change is logged with its source. `xst_clock_from_ntp()` records SNTP's own
  write. Host-tested (`test_xprs_station_host.sh`). Any new "set the time"
  path (GPS, a future command key) goes through this, never `settimeofday`.
- **`ts_epoch()` / `civil()` / `date_prefix()`** — `common/xprs_meshtastic/mt_mesh.c`.
  `ts:YYYY-MM-DD_HH:MM:SS` ↔ seconds since 1970 (Hinnant's civil algorithm,
  no libc `mktime`/`gmtime` and their locale/TZ surprises), and the "[14:26] "
  / "[10-23 14:26] " / "[14:26Z] " prefix for a text whose carrier cannot say
  when it was written. `xauth_ts_epoch` (xprs_auth) is the same parse for the
  app; worth one shared home when a third user appears. The MeshCore bridge
  (`mc_mesh.c`) wants the same gate and prefix — candidate to lift both into
  a `mesh_common`.
- **The mirror gate** — `mt_mesh_on_xprs`: heard traffic crosses to another
  network only under a clock and after `MT_MIRROR_SETTLE_MS`, old news
  (`MT_*_FRESH_S`) is counted and said, not silently dropped. Pattern for any
  bridge that re-airs what it heard.

## Flashing / bench (tools/)

- **`tools/flash-chunked.sh <port> <bin> <offset>`** — writes 256 KB at a time
  with per-chunk retry; the reliable way onto a native-USB S3 (T-Deck/T-Dongle)
  whose single-write flash dies partway, and the only way to recover a
  boot-looping one (the churning USB device fails a plain `pio upload`).
