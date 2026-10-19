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

## Open items

- User decision: Heltec profile A or B.
- T-Deck on-air validation of sliced mode (board not currently connected).
- epaper-1in54 pre-existing flash overflow (predates xwifi).
- Published-image default profile per board once the Heltec decision lands.
