# Making the ESP32-C3-mini a full XPRS station

**Status: planned, not started.** Nothing in here has been implemented. It is
written down now because the firmware tree is being worked on elsewhere and this
board's work must not collide with it — see "Sequencing" at the end.

## What this board is to become

A **full XPRS station**: WiFi STA *and* SoftAP carrying the web chat, **BLE
enabled**, ESP-NOW and LAN bearers, the XPRS index, the station HTTP API,
`xprs_health`, `xprs_config` and signed OTA.

**No Reticulum stack.** No display, no SD card, no LoRa.

The chip is capable of all of it — `SOC_BT_SUPPORTED 1`, `SOC_BLE_SUPPORTED 1`,
**`SOC_BLE_50_SUPPORTED 1`**, so it is extended-advertising capable and can join
the BLE5 mesh plane, exactly as `docs/esp32.md` already says of the "S3 / C3
class". BLE is off today only because `CONFIG_BT_ENABLED` is unset.

## What the working firmware actually was — read this first

The old tree was mined before designing anything, and it **overturned a premise**.

**BLE was OFF.** `CONFIG_BT_ENABLED` is unset in all three historical C3 configs
(`geogram/aurora/esp32/sdkconfig.esp32c3_mini:489`,
`geogram/geogram/esp32/…:489`, `geogram-esp32/code/…:483`), and no
`CONFIG_BT_NIMBLE_*` or `CONFIG_BT_CTRL_*` line exists in any of them. The C3 code
path *calls* `geogram_ble_init()` (`src/main.cpp:1534-1546`) but
`geogram_ble.c:2641` returns `ESP_ERR_NOT_SUPPORTED` when BT is off, so the
working firmware logged **"BLE is disabled in this firmware configuration"** on
every boot.

So: **what worked on this board is WiFi SoftAP + web chat over ESP-Mesh-Lite.
Enabling BLE here is new, unproven work, not a restoration.** There is no tested
C3 BLE configuration to copy.

**The authoritative config is the generated one.** `multiboard/sdkconfig.esp32c3_mini`
(md5 `a1ebebc5…`) is byte-identical to the old tree's and is what was flashed.
The fragment now wired up as `SDKCONFIG_DEFAULTS`
(`models/esp32c3-mini/sdkconfig.esp32c3_mini`, md5 `b3d50e76…`) **never fully
reached the build** — ten settings diverge, including:

| setting | fragment | as actually flashed |
|---|---|---|
| `ESP_WIFI_STATIC_RX_BUFFER_NUM` | 6 | **10** |
| `ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | 16 | **32** |
| `ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | 16 | **32** |
| `ESP_MAIN_TASK_STACK_SIZE` | 4096 | **8192** |
| `FREERTOS_TIMER_TASK_STACK_DEPTH` | 6144 | **4096** |
| `LWIP_MAX_SOCKETS` | 12 | **16** |
| `MESH_LITE_MAXIMUM_LEVEL_ALLOWED` | 3 | **6** |

Anyone "restoring" the fragment's numbers would be flashing a configuration that
never ran. This is the same trap `docs/esp32.md` records for the T-Dongle.

**Measured on the working board:** free heap **~150 KB** after boot
(`geogram-esp32 docs/context.md`), max 4 SoftAP clients, ~2-3 KB per WebSocket.

**The chat page is large and lives in `.rodata`** — no filesystem, no partition.
`chat_page.c` is 202 KB of source serving **≈181 KB** per `GET /`, of which
**146,220 B is a base64 nostr-tools bundle**, sent in 1 KB chunks so nothing is
copied to heap. On a 4 MB part that is a real slice of the app slot.

**The C3 that ran the chat had a single 4,128,768 B `factory` partition**
(`geogram-esp32/code/partitions.csv`), not the dual-OTA table. It had **2.1× the
app space** the current shared table gives it. Sizing below has to respect that.

### Hard-won fixes to carry across, not rediscover

- **`FREERTOS_TIMER_TASK_STACK_DEPTH`**: *"Default 2048 is too small — mesh-lite
  timer callbacks cause stack overflow. 4096 still overflows when phone
  connects, try 6144."* (Moot if mesh-lite is dropped — but the phone-connect
  path is what overflowed it.)
- **Do NOT set `CONFIG_BRIDGE_SOFTAP_SSID_END_WITH_THE_MAC`** — *"this breaks
  phone auto-connect."* Learned twice: added, then reverted.
- **Component CMake must gate on `IDF_TARGET`, not `CONFIG_*`** —
  *"component REQUIRES are resolved before CONFIG_\* is applied, so a
  CONFIG-guarded require is unreliable."* Directly relevant to Stage 1.
- **404s must close the socket** — captive DNS points every domain at the AP, so
  background phone apps exhaust the socket pool. The handler returns `ESP_FAIL`
  deliberately.
- **The HTTP server has ONE worker task**, and on the C3 it is unpinned on the
  only core.
- **Root election is a hard-coded MAC threshold** (`0xe2d800`) tuned to two
  specific boards; any third C3 lands on whichever side its MAC falls. Mesh-only,
  so it disappears with Gen-2.

## Where this board actually stands

Two facts that have to lead, because both are easy to get wrong from the current
tree alone:

**1. It does not compile.** `multiboard/README.md` records it and scopes the
cause honestly: *"components pulling in NimBLE and the message store on boards
whose configuration does not enable them"*. `multiboard/src/CMakeLists.txt:6`
requires `geogram_ble geogram_ble_aprs geogram_ble_hello` unconditionally;
`common/geogram_ble_hello/CMakeLists.txt` requires `bt geogram_msgstore
geogram_xprsindex geogram_xprs`; and `ble_hello.c` / `ble_aprs.c` contain **zero**
`CONFIG_BT_ENABLED` guards, unlike `common/geogram_ble/geogram_ble.c` which
guards every entry point. Errors: `msgstore.h`, `nimble/nimble_port.h`.

**2. It worked before, and that firmware still exists.** This is the single most
important input to the work and the reason Stage 0.5 exists. Do not design the
BLE/WiFi configuration from first principles.

## The constraints that make this board different

| | T-Deck (where the current design was proven) | ESP32-C3-mini |
|---|---|---|
| cores | 2 | **1** (`SOC_CPU_CORES_NUM (1U)`) |
| PSRAM | 8 MB octal | **none** — `SOC_SPIRAM_SUPPORTED` is not defined for esp32c3 |
| flash | 16 MB | **4 MB** |
| generation | Gen-2, `xapp_run()` | Gen-1 `multiboard/`, no `xapp_run()` |
| ISA | Xtensa | RISC-V |

### Single core is the dominant risk, and BLE makes it worse

`docs/esp32.md` is largely a document about which processor work runs on. On the
T-Dongle the BLE controller, the NimBLE host, WiFi and `app_main` sit on core 0
*because* the index writer, the relay task and httpd are pinned to core 1. The
measured consequence of getting that wrong: SD writes on core 0 → reachable
**1 of 96**; the same writes on core 1 → **178 of 182**.

**On the C3 there is no second core.** The BLE controller, NimBLE host, WiFi,
lwIP, httpd, both bearers and the index writer all share one CPU. Every "pin it
to core 1" remedy in that document is unavailable here, and this board will need
its own tuning — drain cadence, task priorities, BLE scan duty — arrived at by
measurement, not by copying the T-Dongle.

Concretely, `taskVALID_CORE_ID(x)` is `x >= 0 && x < configNUMBER_OF_CORES`;
unicore makes that 1, and `CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2` is set
for this board, so **`xTaskCreatePinnedToCore(..., 1)` asserts and aborts at task
creation**. Eight first-party tasks do exactly that today — in `xprs_script`,
`xprs_app` (`idx`), `geogram_xprsindex` (writer), `geogram_rns` (`rns_tcp`),
`geogram_radio_tx`, `geogram_sa818`, and two in the T-Dongle's `main.c` — and
**no component in the tree checks core count**: zero hits for
`CONFIG_FREERTOS_UNICORE` or `SOC_CPU_CORES_NUM` across `common/`.

### The RAM picture is the encouraging part

Measured from the **shipped** C3 binary
(`/home/brito/code/geogram/geogram/esp32/firmware/geogram-ESP32C3-mini.elf`):

| | bytes |
|---|---|
| `.flash.text` | 922,436 |
| `.flash.rodata` | 409,760 |
| **flash image** | **≈ 1,332,452** |
| `.dram0.dummy` + `.data` + `.bss` | **176,900** |
| DRAM region | 393,216 |
| **left for heap** | **≈ 216,316** |

For contrast, the T-Dongle-S3 has 345,856 B of DIRAM with 231,551 static —
114,305 left, and a steady-state free heap around **14,304**.

The ~216 KB above is a static-linkage figure. The **observed** number on the
working board is **~150 KB free heap after boot** (`geogram-esp32
docs/context.md`), with mesh-lite running and BLE off. Treat 150 KB as the real
starting point: BLE, the index and the bearers all come out of it, and mesh-lite
going away gives some back. Either way the C3 has roughly ten times the
T-Dongle's steady-state headroom, which is why an internal script pool is
conceivable here at all despite there being no PSRAM.

---

## Stages

### Stage 0 — check a regression in the shared tree first

`multiboard/components` is a **symlink to `../common`**, and ESP-IDF compiles
every component found there regardless of `REQUIRES` (proof:
`geogram_epaper_1in54` and `geogram_sx1276` appear in a T-Dongle build
directory). Recent work added `geogram_wrench` (665 KB of C++), `xprs_script` and
`xprs_assets` to `common/`, and multiboard demonstrably **whole-archives** its
component libraries.

`multiboard`'s `tdongle_s3` is the one env that still builds, at **94.5 % of its
1,966,080 B slot, 108 KB free**. If `libgeogram_wrench.a` links there it is a
third of the remaining headroom.

- Build `multiboard -e tdongle_s3`; compare `idf_size.py` and `--archives`
  against the committed 1,858,352 B baseline.
- **Report the number before changing anything.**

Fix options if it does link: `EXCLUDE_COMPONENTS` in `multiboard/CMakeLists.txt`,
or a Kconfig gate so the new components compile to nothing unless a board opts in.

### Stage 0.5 — mine the working firmware — **DONE**

Findings are folded into "What the working firmware actually was" above, and they
changed the plan: BLE was never enabled on this board, the authoritative config
is the generated one rather than the fragment now wired up, the chat page is
~181 KB of `.rodata`, and the firmware that worked ran on a single 4,128,768 B
`factory` partition rather than dual OTA.

The sources, kept here so nobody has to find them again:

**This board already worked reliably, after a lot of testing.** The old tree is
evidence, not history: its settings encode fixes whose reasons are written
down nowhere else.

Sources, both on this machine:

- `/home/brito/code/geogram/geogram/esp32/sdkconfig.esp32c3_mini` — a
  **generated** config at project root. Per `docs/esp32.md`'s own rule, the
  generated file at the root is what the build used; the `boards/` fragment is
  only a seed. **This file is the working configuration.**
- `/home/brito/code/geogram/aurora/esp32/` — the tree this repo was imported from
  ("Import the ESP32 firmware from aurora, arranged by board").
- `firmware/geogram-ESP32C3-mini.elf` — the shipped binary measured above.
- `flash-c3.sh` in both trees: the C3 had its own flashing script, i.e. it was in
  routine use.

Extract and carry across **verbatim**, rather than re-deriving:

1. Every BT/NimBLE and WiFi buffer setting in the working generated config.
   **Where these differ from the T-Dongle reclaim table in `docs/esp32.md`, the
   C3's own numbers win** — they are the ones tested on this silicon.
2. How BLE and the SoftAP coexisted on one core: task priorities, scan and
   advertise duty, any `esp_coex_*` / `CONFIG_SW_COEXIST_*`, any yields or delays
   added for the C3 specifically.
3. Whether the old tree pins anything to core 1, and how the C3 survived it —
   guarded, avoided, or never reached.
4. How the web chat was served: which component, embedded rodata or a
   filesystem, and how the captive-portal DNS was arranged.
5. Any comment containing "must", "do not", "otherwise", "crash", "overflow" or
   "workaround" near C3-relevant code. Those are the hard-won fixes.

Remaining deliverable: none — the section above is that record. Re-read it before
Stage 2 rather than re-deriving anything from the current tree.

### Stage 1 — make the C3 compile again (self-contained)

- Guard `ble_hello.c` / `ble_aprs.c` with `#if CONFIG_BT_ENABLED`, matching what
  `geogram_ble.c` already does, so they become stubs where BT is off.
- Make those `APP_REQUIRES` entries conditional, in the `if(CONFIG_GEOGRAM_BOARD_*)`
  style already used for `geogram_ftp` and the T-Dongle's stores.
- Verify `esp32c3_mini` builds; re-test `heltec_v3` and `esp32_generic`, which
  `multiboard/README.md` says fail the same way.
- **Do not touch `multiboard/partitions.csv`** — it is shared by all eight envs.

This stage keeps the C3 on Gen-1 and does not enable BLE; it only stops the build
breaking. Update the build-status table in `multiboard/README.md` to match.

### Stage 2 — a Gen-2 `models/esp32c3-mini/firmware/` project

Mirror `models/tdeck/firmware/` and `models/m5stack-core/firmware/`: a
`platformio.ini`, `partitions.csv`, `sdkconfig.defaults`, a `components/`
directory of symlinks into `common/`, and a `src/main.c` that fills one
`xapp_board_t` and calls `xapp_run()`.

**Symlink:** `xprs_app`, `xprs_api`, `xprs_config`, `xprs_health`, `xprs_ota`,
`xprs_auth`, `xprs_hotspot`, `xprs_station`, `geogram_wifi`, `geogram_xprs`,
`geogram_xprsbearer`, `geogram_xprsnow`, `geogram_xprslan`, `geogram_xprsindex`,
`geogram_xprsid`, `geogram_xprssig`, `geogram_nostr`, `geogram_ble`,
`geogram_blemesh`, `geogram_common`.
**Deliberately not: `geogram_rns`**, and no display, LoRa or SD components.

**BLE on — and understand that this is the riskiest thing in the plan.**

There is no working C3 BLE configuration to copy: the board has never run BLE.
Worse, `docs/espnow.md` records a measured result that bears directly on it —
*"SOLVED: the BLE controller takes the radio from an unassociated station"*:

| BLE controller | station | rx |
|---|---|---|
| absent | never associated, ch 6 | 25 of 25 |
| **up, scanning** | never associated, ch 6 | **1 of 36** |
| **up, scan cancelled** | never associated, ch 6 | **0 of 30** |
| **up, `coex wifi` preference** | never associated, ch 6 | **0 of 30** |
| up, idle | **associated**, ch 1 | 24 of 24 |
| stopped and deinited | never associated, ch 6 | 25 of 25 |

*"With the BLE controller running, a WiFi station that is not associated receives
nothing… it is not the scan. Cancelling the scan does not give the radio back.
The controller merely being up is enough."* And
`esp_coex_preference_set(ESP_COEX_PREFER_WIFI)` does not help.

**The nuance that makes this survivable, and the thing to test first:** the
document explains the mechanism as *"association is what keeps the WiFi side
scheduled: it has beacons it may not miss."* A **SoftAP transmits its own
beacons**, so an AP-mode C3 should keep the WiFi side scheduled the same way an
associated station does. That is a reasoned expectation, **not a measurement**,
and this board is single-core, which the S3 experiments were not.

So the very first BLE test on the C3 is: bring up the SoftAP, serve the chat,
enable the BLE controller, and check the AP still serves and ESP-NOW still
receives. If it does not, the options are BLE **or** the AP, not both — and that
is a product decision, not a tuning exercise.

Ordering rules already learned on the S3 and worth applying from the start
(`src/main.cpp:1553-1560`, `:1588`, `:1698`):
- **Start BLE only after WiFi has finished connecting** — *"an active BLE
  scan/advertise starves the WPA2 4-way handshake and DHCP."*
- **`esp_wifi_set_ps(WIFI_PS_NONE)`** — *"with BLE sharing the radio, modem sleep
  made the STA miss beacons (bcn_timeout) and drop."*
- **`esp_coex_preference_set(ESP_COEX_PREFER_BALANCE)`**, not `PREFER_WIFI`.

Buffer settings: the nearest tested precedent for "BLE beside a SoftAP on a
no-PSRAM chip" is the **KV4P** fragment (`boards/sdkconfig.kv4p:41-63`) —
observer + broadcaster only, no central, `MSYS_1/2_BLOCK_COUNT=6`,
`HOST_TASK_STACK_SIZE=4096`, `MEM_ALLOC_MODE_INTERNAL`, and WiFi cut to 6/16/16.
Prefer that over the T-Dongle table below, which is tuned for a dual-core S3:

```
CONFIG_BT_ENABLED=y                       CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT=8     CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT=8
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=5120
CONFIG_BT_CTRL_BLE_MAX_ACT=3              CONFIG_BT_CTRL_SCAN_DUPL_CACHE_SIZE=20
# CONFIG_BT_NIMBLE_ROLE_CENTRAL is not set
```

Carry the mesh trap too: **`CONFIG_BT_CTRL_BLE_SCAN_DUPL` must be `n`** if this
board is to hear repeat adverts from fixed-address phones (the controller dedups
scan reports by address, so a phone is reported once per boot and then never
again).

**WiFi AP + web chat.** `xprs_hotspot` links at **39,504 B** on the T-Deck
(`chat_page.c` is 42 KB of source), and `xprs_app` already wires
`xprs_hotspot_serve_page()` and `xprs_hotspot_start()` onto the same httpd as the
API. AP+STA is the normal Gen-2 arrangement.

**Headless needs one upstream fix.** `xapp_run()` calls
`board->display_init(&w, &h, &lcd)` **unconditionally** at
`common/xprs_app/xprs_app.c:2679`. It handles a failing *return* ("display init
failed — running headless") but not a NULL pointer. Add the NULL guard so
`.display_init = NULL` is a supported board shape; it helps any headless station.

**Going Gen-2 drops ESP-Mesh-Lite**, and with it the
`mesh_lite → iot_bridge → esp_modem` chain that drags `std::string` and ~318 KB of
libstdc++ into the Gen-1 image. Large win on a 4 MB part, and a deliberate change
of what this board *is*: it stops being a mesh-lite relay and becomes an XPRS
station. Confirm that is wanted before starting.

**Partition table**, sized against the measured 1,332,452 B image:

```
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x6000
otadata,  data, ota,     0xf000,   0x2000
phy_init, data, phy,     0x11000,  0x1000
ota_0,    app,  ota_0,   0x20000,  0x180000    # 1,572,864
ota_1,    app,  ota_1,   0x1a0000, 0x180000    # 1,572,864
storage,  data, fat,     0x320000, 0xc0000     #   786,432  index, via /idx
script_a, data, 0x41,    0x3e0000, 0x10000     #    65,536  only if Stage 4 runs
script_b, data, 0x41,    0x3f0000, 0x10000     #    65,536
```

Fills 4 MB exactly. `xprs_app.c:1857` already mounts `storage` at `/idx` with
`esp_vfs_fat_spiflash_mount_rw_wl()`, so the index works with no SD card — the
M5Stack does exactly this.

**The sizing is genuinely uncertain and must be measured before much code is
written.** The arithmetic pulls both ways:

| | bytes |
|---|---|
| measured working C3 image (includes the ~181 KB chat page) | 1,332,452 |
| **+** BLE (`libbt` 74,433 + `libbtdm_app` 74,725, measured on S3) | +149,158 |
| **−** ESP-Mesh-Lite, `iot_bridge`, `esp_modem` and the libstdc++ they drag | −? |

Adding BLE alone gives ~1,481,610 — **94 % of a 1,572,864 slot**, which is too
tight. It only works if dropping mesh-lite gives back more than it costs, and
that subtraction is **unmeasured on this chip**. Note the old C3 chat firmware
ran on a **single 4,128,768 B `factory`** partition and had no such pressure.

If the measurement comes out badly there are three honest exits, in order of
preference: shrink `storage` and grow the slots; drop the 146 KB base64
nostr-tools blob out of `.rodata` (it is the single largest item in the chat page
and the same "assets belong in a partition" argument `docs/esp32.md` already
makes for the splash); or give up dual OTA and go single-`factory` like the
firmware that worked, accepting USB-only updates.

Also: put real numbers into this board's `README.md`, and fix
`docs/context.md:15`, which still calls the C3 the "Primary development board"
while the top-level `README.md` has moved to the T-Dongle-S3.

### Stage 3 — baseline the station before any VM

The C3 has **no build artifact and no measured heap figure** in this repo; the
only numbers are a hand-written estimate at `docs/mesh-networking.md:595-606`
(~48 KB of 400 KB), for a configuration without BLE.

Use the instruments that already report `MALLOC_CAP_INTERNAL` — `heap_mark()`,
`xh_heap_floor()`, `/api/diag`. (On a PSRAM board `esp_get_free_heap_size()`
counts external memory and the heap-floor alarm can never fire; that was fixed
during the T-Deck work and the same instruments serve here.)

1. `idf_size.py` flash + DIRAM; headroom against `ota_0`.
2. Internal free heap at each `heap_mark()`, at steady state, and **min-ever**,
   with BLE and the SoftAP both up — the worst case.
3. Reachability: one ping a second for three minutes, reported *n* of *m*, with
   no serial port open.
4. The roster comes up complete: `station up: http api+ lan bearer+ esp-now+ …`.
5. The web chat actually serves over the SoftAP, as it did before.

**Gate for Stage 4:** ≥ 60 KB internal free at steady state, largest block
≥ 16 KB, and reachability at the documented level. Below that the C3 is a good
XPRS station that simply does not get scripts, and the work stops here — a
perfectly acceptable outcome.

### Stage 4 — only if Stage 3 passes: port the script host

The Wrench host (`common/xprs_script/`, `common/geogram_wrench/`) works on the
T-Deck: +20,956 B flash, 12 KB internal RAM, signed bundles verified from flash,
90/90 pings under an infinite script. **That last number depended on a second
core and does not transfer to this board.** See `docs/esp32.md`, "Scripts
(Wrench)".

Changes confined to `common/xprs_script/`, all keeping T-Deck behaviour identical:

- **Core pinning:** compile-time choice — core 1 where `SOC_CPU_CORES_NUM > 1`,
  otherwise plain `xTaskCreate` (no affinity). Log which was taken, and state in
  `xprs_script.h` that on a unicore part the core-0 isolation this codebase is
  built around is **unavailable**, leaving only priority and time slices. Apply
  the same guard to the other eight pin-to-core-1 sites, any of which would
  abort this board.
- **Pool:** an internal-RAM mode when there is no PSRAM, default far smaller
  (16–32 KB, Kconfig), still behind the existing hard cap in `xs_alloc()`. Keep
  refusing to start if it cannot be claimed — never silently take memory the
  radios need.
- **Slice budget:** lower `XS_SLICE_INSTRUCTIONS` and measure. On the T-Deck this
  was cosmetic; here it is the only thing between a script and the radios.

---

## Verification

1. **Stage 0:** `idf_size.py --archives` on `multiboard/tdongle_s3` vs the
   1,858,352 B baseline; report the `libgeogram_wrench.a` / `libxprs_script.a` rows.
2. **Stage 1:** `pio run -e esp32c3_mini` succeeds; `heltec_v3` and
   `esp32_generic` re-tested; `tdongle_s3` still builds and has not grown.
3. **Stage 2:** image fits `ota_0` with headroom; `/api/status` and `/api/diag`
   answer; roster complete; the M5Stack (which exists to be a second voice on the
   air) hears the C3 over LAN and ESP-NOW; BLE advertises and a phone sees it;
   the web chat loads over the SoftAP.
4. **Stage 3:** the five measurements above, recorded in `docs/esp32.md` under a
   dated C3 heading, with the single-core caveat stated in the text.
5. **Stage 4 (hard stop):** reachability idle vs a deliberately infinite script,
   **with BLE and the AP up**. `common/xprs_script/spike/spike.w`'s `hot()` and
   `xs_spike(seconds)` already do this. If a runaway script measurably costs the
   station packets on a single-core part, the VM does not belong on it.
6. Host suites stay green throughout — they are target-independent:
   `common/xprs_script/test/test_bundle_host.sh`,
   `common/geogram_wrench/test/test_wrench_host.sh`,
   `common/xprs_assets/test/test_xasset_host.sh`.

## Risks

| risk | mitigation |
|---|---|
| **One core carries BLE controller + NimBLE host + WiFi + lwIP + httpd + two bearers + index writer** | The largest unknown. Stage 0.5 recovers how the working firmware did it; Stage 3 measures reachability and min-ever heap with everything up. `docs/esp32.md`'s "pin it to core 1" answers do not exist here. |
| Recent additions to `common/` may already have grown `multiboard/tdongle_s3` | Stage 0, before anything else. |
| Eight `xTaskCreatePinnedToCore(..., 1)` sites abort on any unicore board | Stage 4 guard; nothing in the tree checks core count today. |
| Image does not fit 1,572,864 B | Measured early in Stage 2; grow slots and shrink `storage`. |
| **BLE + SoftAP has never run on this board, and the BLE controller is measured to take the radio from an unassociated station** (1 of 36, and coex preference does not help) | Test it first, before building anything on top: AP up, chat served, controller enabled, check the AP still serves and ESP-NOW still receives. A SoftAP sends its own beacons so it *should* stay scheduled — reasoned, not measured. If it fails, it is BLE **or** the AP, and that is a product decision. |
| Dropping ESP-Mesh-Lite changes what this board is | Confirm before Stage 2. |
| Re-deriving what was already solved | The whole reason Stage 0.5 comes before Stage 2. |

## Sequencing

The shared firmware tree is being changed by other work in progress —
`common/geogram_xprsindex/`, `common/xprs_app/` and `common/xprs_api/` among
others. **Stages 1, 2 and 4 all touch shared code** (`geogram_ble_hello`,
`xprs_app.c`'s display guard, `xprs_script`), so they must wait for that work to
land rather than race it.

Stage 0 and Stage 0.5 do not: Stage 0 is a build and a measurement, Stage 0.5 is
reading an entirely separate tree. **Both can be done now**, and Stage 0.5 is the
one that most improves everything after it.
