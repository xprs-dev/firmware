# Modifying the D340W firmware (reobell SD-boot hook)

Reproducible pipeline to add a one-time boot hook so the camera runs an editable
XPRS daemon from the SD card — then all logic lives on the SD, no more reflashes.

## What the modification does

A single hook (`firmware-hook.sh`) is appended to rootfs
`/etc/init.d/start_app`. At boot it waits for the SD card and runs
`<sd>/reobell/boot.sh` if present — otherwise nothing changes. Everything else
in the firmware is byte-identical to stock (verified).

This hook variant is one of two builds; the other bakes the whole reobell
payload into the rootfs (firmVer 4664, the version actually deployed). See
`flashing.md` (sections 1-6 for this hook, section 7 for the baked build) and
`tool/` for the reusable Docker image and the baked-build script.

## Build the patched .pak (no device, no host root)

`tool/fw_repack_hook.sh` runs the whole thing in a `debian:12` container. It
takes the stock pak that `stock/fetch-stock.sh` downloads and writes the
patched one into `tool/work/`. The same steps by hand, and the measurements
behind them, are in `flashing.md`: split the stock pak (pakler), extract the
rootfs UBIFS as root (ubi_reader), append the hook to `etc/init.d/start_app`,
rebuild only the rootfs UBIFS + UBI with the D340W geometry, bump the version
in `version_file` and `version.json`, and recompute the pak header CRC. What
the script does:
1. Splits the stock pak (pakler).
2. Extracts the rootfs UBIFS **as root inside Docker** (preserves perms, owners,
   device nodes, symlinks — a non-root extract would corrupt the rootfs).
3. Appends the hook to `start_app`.
4. Rebuilds **only** the rootfs UBIFS + UBI with the D340W-proven params
   (`mkfs.ubifs -m 2048 -e 126976 -c 239`, lzo; `ubinize -m 2048 -p 131072`),
   leaving the `app`, kernel, uboot, etc. sections untouched.
5. `pakler -r` re-inserts the section and fixes the header CRC.
6. Verifies: patched pak passes CRC, and a re-extract shows the tree differs
   from stock **only** in `start_app`.

Params confirmed against the community repacker `gabest11/reolink_firmware_patcher`
(which carries D340W boot logs) and this unit's own UBI/UBIFS headers and the
D340W boot log: PEB 131072, LEB 126976, min-I/O 2048, VID offset 2048, lzo.

## Prepare the SD card

Copy `payload/reobell/` to the SD root and edit `reobell/config`
(admin password, callsign). See `payload/reobell/README.md`.

## Flash — WORKS via the web UI (done 2026-09-16)

The full, validated build-and-flash procedure is in **`flashing.md`** (same
directory). Summary: rebuild the rootfs UBIFS with the hook, bump the version in
both `version_file` and `version.json`, fix the pak header CRC, give the file the
official name with a newer `<VER>`, and upload it in the web UI. There is NO
signature and NO content-hash gate; the camera only checks pak magic, the header
CRC (a non-standard crc32 variant, reproduced in `flashing.md`), board type,
version, and that the rootfs is a mountable UBIFS.

The one non-obvious pitfall that cost the most time: **the browser must be able
to read the .pak file.** A snap/flatpak-confined Chromium cannot read files under
`/tmp`, so it uploads an empty file and the camera "Fails" the upgrade with no
useful error. Put the pak under `$HOME` (or any browser-readable path). The
modified reobell firmware was flashed this way and the camera booted it
(firmVer became `v3.0.0.4663_2509161282`, streaming intact).

- **Keep the stock `.pak`** for reflash/recovery.
- See `flashing.md` for the reusable build tooling (docker `reobell-mtd`), the
  qemu/nandsim validation, and the exact CRC formula and node offsets.

## Recovery if a flash goes wrong

The rootfs mounts read-only from `ubi0:rootfs` on `mtd6`; a bad UBIFS = no boot.
- UART pads are near the image sensor. 3V3 TTL serial, 115200 8N1 on `ttyS0`.
- Interrupt U-Boot at power-on, then TFTP the stock sections back (loader/uboot
  are untouched by this mod, so U-Boot itself remains intact and recoverable).
- Simplest: re-flash the stock `.pak` from U-Boot / the app if it still boots.

## Reverting

Flash the stock `.pak`. The SD `reobell/` folder is inert without the hook.
