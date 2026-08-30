# ESP32-C3 Mini

A small RISC-V board. Built by the shared multi-target project.

## What is in here

| | |
|---|---|
| Chip | `esp32c3` |
| `board.yml` | the catalogue entry, machine-readable (`docs/catalog.md`) |
| `sdkconfig.esp32c3_mini` | this board's ESP-IDF configuration, read by the shared build |
| `docs/` | anything true of this board and not of the others |
| `hardware/` | pinouts, photos, enclosures, 3D prints -- empty until there are some |

## Planned work

`docs/station-plan.md` is the implementation plan for turning this board into a
full XPRS station — WiFi STA + SoftAP with the web chat, BLE on, ESP-NOW and LAN
bearers, the index, signed OTA; no Reticulum, no display, no SD.

**Not started, and partly blocked**: most of it touches shared code in `common/`
that other work is changing. Read the plan's "Sequencing" section before picking
it up.

Things worth knowing even if you never do the work:

- **This board does not currently compile** (`msgstore.h`,
  `nimble/nimble_port.h`), and has not for a while — see the build-status table
  in `multiboard/README.md`.
- **It worked before**, and that firmware is still on disk at
  `/home/brito/code/xprs/xprs/esp32/` (and `xprs-esp32/`, the tree where
  this was the primary board), including a shipped
  `firmware/xprs-ESP32C3-mini.elf` — **1,332,452 B** of flash image,
  **176,900 B** of static RAM, **~150 KB** free heap after boot.
- **What worked was WiFi SoftAP + web chat over ESP-Mesh-Lite, with BLE OFF.**
  `CONFIG_BT_ENABLED` is unset in every historical C3 config; the firmware logged
  "BLE is disabled in this firmware configuration" on every boot. Enabling BLE on
  this board is new work, and `docs/espnow.md` records that a running BLE
  controller can take the radio from an unassociated WiFi station. The plan says
  what to test first.
- **The authoritative config is `multiboard/sdkconfig.esp32c3_mini`** (the
  generated one), not the fragment in this directory — ten settings diverge and
  the fragment's numbers never reached a build.

## Building it

The shared multi-target project builds this board:

```sh
cd ../../multiboard
~/.platformio/penv/bin/pio run -e esp32c3_mini
```

One `src/main.cpp` serves all eight targets in that project; what makes this
board itself is `sdkconfig.esp32c3_mini` here and the `xprs_model_*` component it
selects from `common/`.

