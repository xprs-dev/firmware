# Where the image goes, and what can be extracted

*Measured on the T-Deck, 2026-08-21, `idf_size.py --archives` and `--files`.*

`tinynimble` came from one observation: **NimBLE was 64,632 bytes of host text
serving six commands and one event.** That shape — a large general-purpose
library reached through a tiny surface — is worth looking for deliberately,
because it is where the disproportionate wins are.

This page lists the other places the same test finds something, ranked by what
they would return against what they would cost. It is a survey, not a plan;
nothing here is committed to.

## The test

Two numbers per component: **how big is it linked**, and **how much of it does
the firmware actually reach**. A library is a candidate when the second is a
small fraction of the first *and* the used surface is stable and well
understood. It is NOT a candidate merely for being large — lwip, net80211 and
the BLE controller are all large and all used broadly.

## Candidates

### 1. LVGL — 274,925 B linked (50,280 of it `.bss`), five widget types used

The strongest parallel to NimBLE. Across `xprs_ui` and `xprs_ui_mini` the
firmware creates exactly:

| widget | uses |
|---|---|
| `lv_label` | 29 |
| `lv_obj` | 20 |
| `lv_line` | 4 |
| `lv_chart` | 2 |
| `lv_table` | 1 |

What is linked anyway: **four Montserrat fonts totalling 56,274 B** (10, 12, 14,
20), `lv_theme_default` 11,404, `lv_btnmatrix` 4,927 — a widget never created
anywhere in the tree — plus the drawing, masking and scrolling machinery of a
general-purpose toolkit. `lv_mem.c.o` is 50,608 B of `.bss`: the pool, sized per
board and already tuned.

**Cheap first, as with NimBLE's roles.** Dropping unused font sizes and
disabling unused widgets in Kconfig costs nothing and is reversible; do that and
re-measure before considering anything larger. Only if a real gap remains is a
minimal renderer worth discussing — and note the two screens are a 160×80 text
dashboard and a labels/table/chart panel, both of which a few hundred lines
could draw straight through `st7789_flush()`.

### 2. `xprs_hotspot` — 39,504 B, nearly all one HTML page in `.rodata`

The web chat is a string literal compiled into the image. The Gen-1 version was
worse: 202 KB of source, of which **146,220 B is a base64 nostr-tools bundle**.

This one already has its answer built. `common/xprs_assets/` reads blobs from a
raw flash partition with `esp_partition_read`, costs about **8 bytes of `.bss`
and no heap**, and works before any filesystem is mounted. Moving the page there
takes it out of the app image entirely and makes it replaceable without a
reflash. See `tools/mkassets.py`.

### 3. mbedTLS elliptic curves — 36,455 B of `ecp_curves.c.o` alone

Plus `ecp.c.o` 8,465, `rsa.c.o` 5,826, `bignum.c.o` 5,236 — **~56 KB**.

The firmware's own crypto does not need this: `xprs_sig` implements
secp256k1 itself, and the mbedTLS calls in our code are only AES-CBC, SHA-256,
base64 and CTR-DRBG. The curve tables come in through **WPA3/SAE**, which is
enabled four ways (`ESP_WIFI_ENABLE_WPA3_SAE`, `SAE_PK`, `SOFTAP_SAE_SUPPORT`,
`WPA3_OWE_STA`).

So this is a **deployment question, not an engineering one**: if the access
points a station meets are WPA2, turning WPA3 off returns ~56 KB for one
Kconfig block. If any deployment needs WPA3, it stays. Worth asking before
anyone tries to shrink it by other means.

### 4. `printf`/`scanf` — 66,830 B, and NOT free

Six objects: `vfprintf` 14,233, `svfprintf` 13,685, `svfiprintf` 10,478,
`vfiprintf` 10,168, `svfscanf` 9,811, `svfiscanf` 8,455.

`CONFIG_ESP_ROM_HAS_NEWLIB_NANO_FORMAT=y` — the ROM already contains a smaller
implementation — but `CONFIG_NEWLIB_NANO_FORMAT` is **not set**, so the build
links its own.

**The catch, checked before recommending it:** the nano formatter has no
floating-point support, and the tree uses `%f`/`%g` in **45 places**. Enabling
it today would silently break every one. This is only a candidate for someone
willing to convert those sites to integer formatting first — which is plausible
(most are coordinates and averages that could be fixed-point) but is real work
with a real chance of changing output people read.

### 5. `libxprs_app.a` — 38,169 B of `.bss`, and it is ours

The largest static-RAM consumer after LVGL's pool, and the one place on this
list where the code is entirely under our control. Nobody has audited what is
static in there. On a board whose steady-state internal free heap is under
9,000 bytes, 38 KB of `.bss` deserves a look before any library is blamed.

`libxprs_station.a` adds 15,644 more.

### 6. FatFs + wear levelling — 21,422 B

The XPRS index appends fixed-size records and reads them back by offset. It does
not need directories, long filenames, FAT tables or a wear-levelling layer
designed for arbitrary files — and `docs/esp32.md` already records what FatFs
costs beyond flash: **4 KB of sector cache per open file**, plus the traps about
directory entries and `fsync`.

`common/xprs_assets/` is the existence proof that a raw partition is enough for
read-only blobs. A log-structured append-only store over a raw partition is the
same idea for the index, and would remove the per-file caches as well as the
code.

### 7. `http_parser` — 16,075 B for about ten endpoints

Small enough that it is listed for completeness rather than urgency.

## Not candidates

`net80211` (128,063), `lwip` (105,883), `wpa_supplicant` (63,431), `pp`, `phy`
and the BLE controller `libbtdm_app` (66,474) are large and **broadly used**.
The controller in particular is a binary blob with microsecond deadlines: it is
the floor under any BLE work, which is exactly why `tinynimble` targets only the
host above it.

## The order that has worked twice now

1. **Trim configuration first.** NimBLE's roles, LVGL's fonts and widgets, WPA3.
   Free, reversible, and it often closes most of the gap — trimming NimBLE's
   roles alone took the tinynimble flash saving from a claimed 62 KB down to a
   real 21 KB.
2. **Measure the remainder on the same board, same test, one variable.**
3. **Only then extract**, and keep the original path behind a switch so the
   comparison stays runnable and the decision stays reversible.

---

# What was actually done, and what it returned

*Applied and measured on X3R8XX (T-Deck, `DC:DA:0C:39:F6:54`), 2026-08-21.
Every row was built, flashed and booted; the UI was screenshotted and the
hotspot page fetched over HTTP after each.*

| step | flash | internal RAM |
|---|---|---|
| baseline (`tinynimble`, PSRAM on) | 1,486,873 | 175,052 |
| LVGL: 29 unused widgets off, examples off | 1,429,749 | 174,924 |
| hotspot chat page stored gzipped | 1,409,093 | 174,924 |
| `tinylv_mem`: LVGL's pool to PSRAM | 1,406,809 | 125,756 |
| mbedTLS: WPA3/SAE, RSA/PSA tail, 11 curves off | 1,316,141 | 125,476 |
| `XPRS_PSRAM_BSS` on our own big statics | **1,315,565** | **71,556** |
| **total** | **−171,308 (−11.5 %)** | **−103,496 (−59 %)** |

The number that matters is neither of those columns. It is the internal heap
left at the tightest moment of the boot — just after the hotspot comes up,
which is when the softAP asks for a 752-byte beacon buffer:

| | internal free | largest block |
|---|---|---|
| before | 3,492 | 1,600 |
| after | **70,940** | **31,744** |

## The lesson this run actually taught

**Flash was never the problem, and trimming it does not help.** Two thirds of
the flash saved here (the mbedTLS trim, the gzipped page) bought nothing that
the board could feel. What the board felt was moving *internal DRAM* to PSRAM:
`tinylv_mem` and `XPRS_PSRAM_BSS`, 103 KB between them, and neither one is a
size reduction — the bytes still exist, they are just somewhere that is not
scarce.

There is a sharp demonstration of this in the middle of the table. Applied on
its own, **the mbedTLS trim crashed the board**: `ieee80211_hostap_attach`
took a `LoadProhibited` on every boot, preceded by `wifi: alloc eb len=752
type=4 fail`. Nothing was wrong with the crypto — secp256k1 still derived the
right npub. The station was simply living on 3,492 bytes of internal heap with
a largest block of 1,600, and *any* change to the layout was a coin toss on
that 752-byte allocation. The same config, re-applied after `tinylv_mem` had
freed 49 KB, boots clean every time.

So: when a board is that close to the floor, a build succeeding proves nothing
and a build crashing does not mean the change was wrong. Fix the floor first.

## Detail per step

### LVGL widgets — 57,124 B of flash

35 widgets were enabled; five are ever created (`chart`, `label`, `line`,
`obj`, `table`). 29 of the others are now off in `sdkconfig.defaults`.

`CONFIG_LV_BUILD_EXAMPLES=y` had to go first: the examples are compiled, they
use the widgets being removed, and `-Werror=implicit-function-declaration`
makes that a build failure. They cost no flash — `--gc-sections` drops them,
0 bytes linked — only build time and this obstacle.

The four Montserrat fonts (56,274 B) stay: all four are genuinely referenced
(10 ×7, 12 ×16, 14 ×3, 20 ×1). That is the largest single item left in flash
and it is not waste.

### The hotspot chat page — 20,656 B of flash

38,845 bytes of HTML, JS and a base64 WOFF2 gzip to 18,139. `embed_page.py`
now emits the compressed bytes and `h_page()` sets `Content-Encoding: gzip`;
nothing decompresses on the device. Verified by fetching it from the LAN side
and comparing SHA-256 against the source — byte-identical.

### `tinylv_mem` — 49,168 B of internal DRAM

LVGL's TLSF pool is a plain array in `.bss`, so `CONFIG_LV_MEM_SIZE_KILOBYTES`
is spent whether the UI uses it or not. `LV_MEM_CUSTOM` plus a PSRAM-preferring
allocator (`common/tinylv_mem/`) moves it. The four `LV_MEM_CUSTOM_*` macros
are `#ifndef`-guarded in `lv_conf_internal.h`, so the board's `CMakeLists.txt`
`-D`s them at the LVGL component and the managed component stays unpatched.

Everything in that pool — objects, styles, `lv_mem_buf` scratch — is CPU-only.
The draw buffer is not in it: `xprs_ui` allocates that separately with
`MALLOC_CAP_DMA` and it must stay internal.

### mbedTLS — 90,668 B of flash

Our entire use of mbedcrypto is AES-CBC, base64, CTR-DRBG, entropy, HMAC,
SHA-256, and ECP on **one** curve (secp256k1). The map showed why 97,124 B was
linked:

```
wpa_supplicant/crypto_mbedtls-ec.c   (WPA3/SAE)
  -> mbedtls_pk_parse_subpubkey -> pkparse.c -> rsa.c, rsa_alt_helpers.c
  -> mbedtls_pk_init -> pk.c -> pk_wrap.c -> psa_crypto.c -> the whole PSA
     layer (rsa, aead, cipher, mac, ecp, hash, slot mgmt, storage, ITS)
md.c's generic dispatch -> sha1.c + sha512.c + sha3.c (keccak_f1600, 1,835 B)
```

SAE is what pays for RSA and PSA. Turning it off is a **real capability
loss**: no joining a WPA3-only AP and no WPA3 on our own hotspot. WPA2 is
untouched and is what every XPRS deployment uses; the STA re-joined and got
`192.168.178.140` on the trimmed build. Deleting the block in
`sdkconfig.defaults` is the whole revert.

No TLS is linked on this board at all (no `libmbedtls.a`, no `libmbedx509.a`
in the map), so the CA bundle and PKCS7 were verifying nothing.

### `XPRS_PSRAM_BSS` — 53,920 B of internal DRAM

With LVGL gone from the `.bss` ranking, our own components were at the top:
`libxprs_app.a` 38,221, `libxprs_station.a` 15,652, `libxprs_ui.a` 8,017 —
chat rings, device tables, statistics buckets, table-row scratch. All
task-context and CPU-only, so `EXT_RAM_BSS_ATTR` relocates them with no change
to how they are used. See `common/xprs_common/include/xprs_psram.h` for
what may and may not carry the attribute.

The log ring (`s_logring`, 6,144 B) was deliberately **left internal**: it is
filled from a `vprintf` hook, and a buffer that might be written while the
cache is off is not worth 6 KB.

## Still open

- `libxprs_app.a` still holds `.bss` after the annotated arrays; worth a
  second pass once this one is proven over a longer soak.
- `printf`/`scanf` float support, 66,830 B of flash, blocked by 45 `%f` sites.
- FatFs 21,422 B and `http_parser` 16,075 B — both genuinely used; a reduced
  version would be real work for flash we do not need.
- `E (17716) vfs_fat: open: no free file descriptors` — **fixed**, and it was
  not cosmetic. `CONFIG_SDCARD_MAX_FILES` was 3 while the index holds three
  handles open by construction (`active_fp`, `tail_fp`, and xprs_app's log), so
  the pool was full at rest and every transient open failed. The zone map, the
  statistics rings, the declared mailboxes, the regulars list and 36.11 mail
  carry-forward had all stopped persisting, silently, because every one of
  those writers returns `void`. Raised to 6 and every one of them now logs on a
  failed open. Full arithmetic in the T-Deck's `sdkconfig.defaults`.

  Worth noting for the footprint argument: the extra slots cost **PSRAM, not
  internal DRAM** (`CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y`) — measured 8,263,228 →
  8,252,152 of PSRAM with `heap after hotspot` unchanged at 71,100. On a board
  without PSRAM each slot is ~4 KB of internal DRAM and the old warning holds.
