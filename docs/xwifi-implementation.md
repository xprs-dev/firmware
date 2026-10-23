# xwifi implementation plan

Status date: 2026-10-19. ESP-NOW is deprecated; XPRS-WiFi (xwifi, the CSS
long-range 2.4 GHz bearer, library at `common/xwifi`, bearer glue at
`common/xprs_bearer_wifi8`) replaces it on every published board. This file
records what is already working, the measured memory physics that constrain
the no-PSRAM boards, and the open decision on the Heltec V3 profile.

## What works today (bench-validated)

- Two-way xwifi traffic between the Heltec V3 (first silicon run of the
  esp32s3 port) and the ESP32-C3 relay. Proof on air: the Heltec beacon came
  back digipeated (`t:observation f:X333SM link:wifi8 ... via:X5FZVE`), the
  third station X32ZPN reports `hears:X333SM,X5FZVE`, and the Heltec hears
  X5FZVE at 31 to 38 dB SNR.
- LoRa coexists with xwifi on the Heltec (shared `xb_tick_all` pump).
- C3 relay (`models/esp32c3-mini`, env `c3xwifi`, `src/xwifi_relay.c`): full
  bearer with dedup rings and 13.2.1 cancel, 10.6.1 observation beacon,
  13.1 digipeating. The normal `c3mini` env is untouched.
- Regression builds green: tdeck, tdongle-s3, m5stack-core, generic, c3mini.
  (epaper-1in54 fails on a pre-existing ~11 KB flash overflow unrelated to
  xwifi.)

Key placement wins already landed:

- S3: station state (`xw_station_t`, ~58 KB) and its task stack live inside
  RF sample bank 2 at 0x3FCD0000, which is idle when looks <= 2.
- S3: the down-chirp table lives in RTC RAM (7 KB pool, internal fallback);
  the zero table is deleted.
- C3: station state sits above the bank via `SOC_RESERVE_MEMORY_REGION`.
- App: `xprs_big_calloc` (SPIRAM first, internal fallback) moved the chat
  ring, log ring, index queue and page buffers off .bss and made them lazy.
- Busy-channel refusals (7.3, send returns -2) are retried with random
  backoff instead of dropped.

## The physics (measured, not estimated)

Heltec V3 is an ESP32-S3 with 512 KB SRAM and no PSRAM. The xwifi receiver
and transmitter record raw I/Q into the chip's RF sample banks at fixed
silicon addresses: three 64 KB banks at 0x3FCB0000, 0x3FCC0000, 0x3FCD0000.
That is 192 KB of SRAM the heap can never touch while xwifi is compiled in.

Budget with every trick applied:

- Code in RAM plus static data: about 165 KB (WiFi IRAM options off,
  FreeRTOS and heap functions in flash, LVGL pool on heap, big buffers
  lazy-allocated).
- RF banks: 192 KB reserved (station and stack reuse bank 2, so 66 KB of
  the reservation does useful work).
- Remaining heap pools: about 99 KB.

Full-station runtime demand: 802.11 STA+AP about 45 KB, BLE5 about 20 KB,
screen about 20 KB, webchat/HTTP about 12 KB, archive/index about 25 KB,
LoRa about 7 KB, base plus app about 40 KB. Total about 170 KB against
99 KB available. The gap is 60 to 70 KB and no placement trick closes it:
reserving only two banks returns 64 KB to the heap but pushes the station,
stack and chirp tables back onto it for a net loss. The pre-xwifi full
station fit because ESP-NOW needs no sample banks; the 192 KB is the entire
difference.

The C3 is worse: its single 128 KB bank bisects the one SRAM and caps static
data at about 55 KB, while the full station's statics are about 100 KB
however placed. The relay is what fits, and it already carries any XPRS
wire, messages included.

### The C3 ledger, measured (2026-10-23, board AC:27:6E:74:0C:FC, X3SQS6)

The goal moved: the C3 is to run the FULL station — Reticulum on a public
hub, the hotspot with the chat page, BLE5 and the xwifi mesh — at the same
time ("simultaneous or nothing"). So the paragraph above was re-measured
instead of trusted, on a bench C3 running `c3mini` at this commit with every
subsystem live (hub link to rns.wisco.network:4242 included; 180/180 pings,
page in 0.44 s while all of it ran).

**Statics are no longer ~100 KB, and the picture is tighter but different:**

| | bytes |
|---|---|
| `c3mini` statics (.data+.bss) | 92,600 |
| `c3full` (= c3mini + CONFIG_XPRS_WIFI8, links clean) statics | 113,304 |
| `c3full` heap_start | 0x3FCAE498 — **58,520 into the bank** |
| …of which xwifi itself | 16.9 K .bss + 19.0 K IRAM text + 3.5 K bearer |
| total heap, no bank reserved | 239,776 |
| heap left once the bank is reserved | ~127 K (0x3FCC0000 regions) |
| steady free today, everything on | 32.7 K (min 20.8 K, largest 13.3 K) |

**Runtime cost per subsystem, measured by switching each off on the board
(`cfg set espnow_on/index_on/ble_on no`, reboot, 100 s settle, `/api/diag`):**

| subsystem | steady heap it holds |
|---|---|
| ESP-NOW | 2,456 (it rides the WiFi driver; the driver is the cost) |
| **index/archive (FAT + WL on the 832 K partition)** | **49,952** |
| BLE (tinynimble + controller) | 38,496 |

So the bank's ~113 K price is answered almost entirely by two decisions:
what the C3's archive is (the FAT index machinery holds 50 K of RAM for a
384 K archive), and how much of xwifi's own 58.5 K of below-bank pressure
moves into the bank it reserves (the port already runs staging and the
second-look window in the capture-free lower half; its hot code could run
from the same half's instruction-bus alias, 0x403Axxxx — unproven).
The config floors are already spent: WiFi 6/12/16, TCP 536/2680,
BT_CTRL_BLE_MAX_ACT=2.

## Per-board plan

### T-Deck and other PSRAM boards: full station plus xwifi

The real "preserve everything, swap ESP-NOW for xwifi" target. PSRAM absorbs
the screen, BLE host, FAT caches and WiFi buffers, so the internal-RAM
demand collapses and the banks fit. Work items:

1. Enable `CONFIG_XPRS_WIFI8` in the tdeck profile (build already green).
2. Run xwifi in time-sliced mode (slot_ms > 0) beside the live 802.11
   driver. This path is written but has never run on hardware; validating
   suspend/resume against a live AP association is the main risk.
3. Validate the full list on the bench: BLE5 advertising, STA join to local
   WiFi, hotspot with webchat, and xwifi exchange with the C3 relay.

#### First hardware attempt, 2026-10-23 (env `tdeck_xwifi`, X3R8XX)

Three walls, in the order they fell, and the one still standing. The env is
`models/tdeck/firmware/` + `sdkconfig.xwifi`; stock `tdeck` boots fine on the
same board throughout (X3SEP2), so each wall is the xwifi config, not the
branch.

1. **Static data overran the S3 RF bank.** `memory_layout` aborts at boot when
   statics+IRAM reach 0x3FCB0000. First build: 31.8 KB over. Cleared without
   the flash-code trick by **three reusable placement changes now in the
   shared tree**: the four bearer `xb_t` structs (ESP-NOW, LAN, BLE, wifi8,
   3.4 KB each) moved from `.bss` to `xprs_big_calloc` at start like the LoRa
   bearer already did; the xwifi demodulator's working arrays (~6.4 KB, were
   function statics) moved into a new `xw_scratch_t` the application provides,
   exactly as the station already provides `extra_window` (PSRAM-first on a
   board that has it); and WiFi driver code out of IRAM. Host suites green:
   `test_xprslan_host.sh` (76/76) and all four xwifi `ctest`s.

2. **The system event loop could not start.** `esp_event_loop_create_default()`
   aborted in `wifi_up()` with "create task for loop failed" — its task stack
   is contiguous INTERNAL RAM, and with the 192 KB sample banks reserved and
   BLE + the LVGL DMA draw buffer already taken, there was none left by the
   time wifi_up ran. Fixed in `xprs_app.c`: the loop and `esp_netif_init()` are
   now claimed at the top of `xapp_run()` while the heap is whole, and wifi_up
   tolerates the already-created state. "Boot order is the allocator"
   (docs/esp32.md), applied to the one task that needed it. Helps every xwifi
   board.

3. **Still standing: xwifi's RAM trick vs the BLE controller's deadlines.**
   The S3 port's README lists FreeRTOS- and heap-functions-in-flash as a
   requirement; they buy ~24 KB of below-bank space. But the BLE controller
   runs its link layer to microsecond deadlines (`r_lld_core_init`,
   `btdm_controller_task`), and a flash-cache miss on a FreeRTOS/heap primitive
   it calls stalls it past the interrupt watchdog — a boot loop,
   "Interrupt wdt timeout on CPU0". On the Heltec xwifi profile BT is OFF, so
   the trick is free; on the T-Deck BT is ON (it is goal 3), so the trick is
   fatal. Without it BLE is happy but statics are 24 KB over the bank again.
   So full station + BLE + xwifi on the S3 needs ~24 KB of below-bank relief
   that is NOT the flash-code trick: more `.bss` to PSRAM (the app's big
   statics already carry `XPRS_PSRAM_BSS`; the next candidates are IRAM .text
   that need not be resident, and any remaining internal-only scratch), or the
   BLE controller kept entirely off the flash-cache path. This is the next
   session's work and it is the same shape the C3 faces, one board easier.

### Heltec V3: profile decision pending (user to pick)

- Option A, modal single image: all functions in one firmware, two runtime
  modes switched by config plus reboot. Station mode is BLE5, local WiFi,
  hotspot webchat, screen and archive exactly as before, xwifi off. Radio
  mode is today's lean xwifi set. Needs BT compiled back in, which needs
  about 26 KB more .bss moved to heap; the movers exist (the four static
  `xb_t` bearer structs at 13.6 KB, and the xwifi demod function statics at
  12.4 KB, heap-allocated at first use).
- Option B, bridge profile: hotspot webchat, local WiFi STA, xwifi and LoRa
  simultaneously; no BLE5, no screen, no archive. Covers the end-to-end
  "type in webchat, C3 hears it over xwifi" scenario. Needs both flash
  caches halved (frees 32 KB) and lands within about 5 KB of the ceiling,
  so it may OOM under load. Borderline.
- Option C, lean radio: today's state, xwifi plus LoRa plus console only.

All three keep the same bearer code; they differ only in sdkconfig and
runtime flags.

### C3-mini: the static-RAM verdict, quantified (2026-10-23)

`c3full` (full station + `CONFIG_XPRS_WIFI8`, BT and RNS on) links and its
static data (`.data`+`.bss`) ends **38,560 B past the C3 RF bank** at
0x3FCA0000 — down from 58,520 B before the Stage-2 movers (the four bearer
`xb_t` structs and the demod `xw_scratch_t`, ~20 K, now in the shared tree).
On the unicore C3 the IRAM and DRAM share one SRAM pool with IRAM below
`.data`, so moving code out of IRAM lowers the `.bss` end — but the C3 build
**already** runs `ESP_WIFI_IRAM_OPT=n`, `ESP_WIFI_RX_IRAM_OPT=n`,
`FREERTOS_PLACE_FUNCTIONS_INTO_FLASH` and `HEAP_PLACE_FUNCTION_INTO_FLASH`,
so that lever is spent. No PSRAM, so `XPRS_PSRAM_BSS` is inert.

Where the remaining 38,560 B is, measured (`idf_size.py --archives`):

| static `.bss`/`.data` | bytes | can it leave internal SRAM? |
|---|---|---|
| `libxprs_app` | 16,229 | yes — `xprs_big_calloc` to heap above the bank, as the bearers and the echo/log/idx buffers already do |
| `libxprs_station` | 3,776 | yes, same way |
| `libxprs_diag` + `config` + `nostr` | ~2,356 | partly |
| **`libxprs_index` (the archive store)** | **0** | **n/a — the store holds NO static `.bss`; its ring, queue and page are `big_calloc`'d, and FatFs's 4 KB-per-file sector caches are runtime HEAP, not static** |
| IDF drivers: the BLE **controller** DRAM ~15 K, net80211/lwip/FreeRTOS (already floored) | ~16,000 | no |

So two separate facts, not to be conflated:

- **The static overrun gates the fit, and the archive backend does not touch
  it.** Relocating all of our own `.bss` (~22 K) to heap still leaves ~16 K
  over the bank, and that ~16 K is essentially the BLE **controller**'s static
  DRAM. On this no-PSRAM silicon it has nowhere to go. The measured reading of
  "all four simultaneously" is therefore: **Reticulum + hotspot + xwifi fit the
  bank once our `.bss` is moved to heap; BLE is the ~15 K that does not.**
- **A raw-partition store is a HEAP win (~16 K: the FatFs sector caches and the
  wear-levelling layer), not a static-RAM win.** Worth having for the
  steady-heap budget once the static overrun is solved, and it is the right
  shape for a small device (footprint.md §6), but it is NOT the lever that
  closes the C3 and `libxprs_index` having 0 static `.bss` is the proof.

#### Relocating app `.bss` to heap — the trajectory (2026-10-23)

The gating overrun is `.data/.bss`, so the lever is moving it off the static
image and onto the heap above the bank — `xprs_big_calloc` for file-scope
arrays, `render_scratch()` for a UI render static. Index-accessed arrays
convert with no use-site change (a pointer indexes like the array did); the
only hazards are `sizeof array` and `&array`, neither of which these had.
Done in `libxprs_app`: `s_flow`, `s_peers`, `s_askq`, `s_acked`, `s_meshq`,
`s_hist_alwayson` (claimed in `xapp_run` before `acked_load`, fatal-on-fail
like `s_echo`), and `mm` (the chat-panel render buffer, now `render_scratch`,
so a headless board that never renders pays 4 bytes not 1,692). `libxprs_app`
DRAM 16,229 → ~8,000.

Measured bank overrun (`c3full_nobt` = full station + xwifi, BLE off, the
1+2+4 target), each step the same technique:

| state | `.bss` end past 0x3FCA0000 |
|---|---|
| before any Stage-2 work | 38,560 B |
| + bearer movers + demod scratch (c3full, BLE on) | 32,096 B |
| + BLE off (the ~15 K controller the chip cannot place) | 23,552 B |
| + the six app arrays | 12,128 B |
| + `mm` via render_scratch | **10,432 B** |

So 1+2+4 (Reticulum + hotspot + xwifi, no BLE) is **~10 K from fitting the
bank**, down from 38.5 K, entirely by relocation — no feature dropped but
BLE, which is the piece measured not to fit. The last ~10 K is in other
libraries by the same method: `libxprs_station` 3,776 (`s_sb10` and the
gossip/device tables), `xwifi`'s own `surv.0` 2,112 (the channel-survey
static, belongs in `xw_station_t`), `xprs_ota` `s_body` 1,024 (the push
buffer, used only during an update), and the tail of `libxprs_app`.

The shipping `c3mini` (BLE on, no xwifi) benefits too: static RAM 93,344 →
74,264, and it was reflashed to the bench board and verified running — 20/20
pings, internet up, clock synced, the relocated receive/relay arrays
exercised live with no fault.

#### The static fit was reached — and booting proved the heap cannot (2026-10-23, the definitive verdict)

Continuing the relocation (xprs_station rings, xprs_ota `s_body`, xprs_tz
`s_rx`, xprs_api `row`, xprs_app `kv`, the C3 xwifi port's `li`/`lq`, and the
Reticulum peer table `s_peers`) took `c3full_nobt` to **784 B UNDER the bank**
— the static overrun, 38,560 B at the start, is fully closed.

**Then it was flashed and it boot-loops**, and the crash is the other half of
the ledger, exactly as footprint.md warns ("a build fitting proves nothing
near the floor"). With the 128 KB bank reserved, `heap_init` leaves **784 B
below the bank + 56 KB + 10 KB above it = ~67 KB usable heap**. By the time
`app_main` reaches WiFi — even with **BLE off and the indexer off** — only
**17.6 KB is free, largest block 9.2 KB**, and the WiFi driver needs a
contiguous ~40 KB it cannot get. `ESP_ERR_NO_MEM`, panic.

Why relocation cannot fix this on a no-PSRAM chip: **it is zero-sum.** Moving
a buffer from `.bss` to `xprs_big_calloc` on a board with no PSRAM moves it
from below the bank (where it overran) to the post-bank heap (where WiFi
needs the room) — the same internal-SRAM bytes, relocated, not removed. The
128 KB bank takes its bite of the 400 KB SRAM regardless; what remains
(~67 KB heap, in two non-contiguous islands of 56 and 10 KB) cannot hold the
full station's runtime (WiFi ~40 KB contiguous + hotspot + RNS + the xwifi
station state + the relocated ex-`.bss` buffers), with or without the index's
~16 KB and with or without BLE.

**So the measured answer for the ESP32-C3 full station + xwifi is: it does not
fit, and no placement trick closes it — the bank plus the full-station runtime
heap exceed the chip's SRAM.** This confirms and now quantifies the original
conclusion at the top of this file ("The C3 is worse… the relay is what
fits"): the `c3xwifi` relay fits precisely because it has no WiFi-station,
hotspot, Reticulum or index heap. A board that is to run the full station AND
xwifi needs PSRAM to absorb that runtime — which is the T-Deck, where these
same relocations (big_calloc is PSRAM-first there) help rather than merely
move the problem.

**What stands regardless, and is kept:** the relocation work is a real,
fleet-wide static-RAM reduction (shipping `c3mini` 93,344 → 74,264, verified
on hardware; every xwifi board lighter in `.bss`), and the bearer/demod/
event-loop changes are correct on their own. The C3 itself stays the relay
for xwifi and the full station for everything else, as two envs.

### C3-mini: relay

`c3xwifi` env stays the xwifi answer on this chip. Normal `c3mini` env
remains the ESP-NOW-era full station until ESP-NOW is retired from it.

## Hard-won build rules (keep)

- ESP-IDF component CMakeLists must not branch REQUIRES on `CONFIG_*`: the
  early dependency pass runs without sdkconfig and the else branch always
  wins. Branch SRCS only.
- The esp32s3 and esp32c3 xwifi ports need
  `CONFIG_ESP_PHY_ENABLE_CERT_TEST=y`, and on the S3 also
  FreeRTOS/heap-functions-in-flash and 240 MHz.
- Delete generated `sdkconfig.<env>` files before builds so
  `sdkconfig.defaults` changes take effect.
- `rf_bank_guard.ld` is dead weight in this PlatformIO + IDF 5.2.1
  toolchain (raw .ld ASSERTs never fire); the `SOC_RESERVE_MEMORY_REGION`
  entries are the working guard.

## Resuming on another machine

All xwifi work is parked, saved as follows:

- Firmware: branch `xwifi-wip` of `xprs-dev/firmware` (main carries only
  this plan). The branch holds the bearer (`common/xprs_bearer_wifi8`), the
  app and station edits, the Heltec sdkconfig, and the C3 relay env.
- Library: `xprs-dev/xwifi` (private), branch `main`. The validated S3 and
  C3 ports are committed there; `xw-range` is a side branch.
- Checkout layout: the two repos must be siblings, because
  `common/xwifi` is a symlink to `../../xwifi`:

      ~/code/xprs/firmware   (branch xwifi-wip)
      ~/code/xprs/xwifi

- Builds: `~/.pio-venv/bin/pio run -e heltec_v3` (or `-e c3xwifi` in
  models/esp32c3-mini/firmware). Delete generated `sdkconfig.<env>` files
  first when defaults changed.
- Bench state (this stays on the original machine): Heltec V3 X333SM on
  ttyUSB0 and C3 relay X5FZVE on ttyACM0 are flashed with the lean xwifi
  builds and talking; Heltec NVS has wifi_on, espnow_on, index_on, ap_on
  all set to no.

## Open items

- User decision: Heltec profile A or B.
- T-Deck on-air validation of sliced mode (board not currently connected).
- epaper-1in54 pre-existing flash overflow (predates xwifi).
- Published-image default profile per board once the Heltec decision lands.
