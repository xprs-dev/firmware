# D340W hardware

Everything here was either read off the device over the LAN or found in its
stock firmware. Where a line comes from Reolink's published specification
rather than from the unit, it says so, because the two are not the same kind
of fact.

## Identity, as the camera reports it

`api.cgi GetDevInfo`, from the unit this port was developed on:

| Field | Value |
|-------|-------|
| Model name | Reolink Video Doorbell WiFi |
| Item number | D340W |
| Hardware version | DB_566128M5MP_W |
| Firmware version (stock) | v3.0.0.4662_2508071282, built 2025-08-07 |
| Detail code | S36E1W7T4C0E010A |
| Device type | BELL |
| Channels | 1 |
| Disks | 1 (the microSD slot) |

The hardware string is the part that matters when flashing: the updater
compares it, so an image built for `DB_566128M5MP_W` is refused by anything
else and the pattern in a pak's filename has to carry it.

## What it is made of

Read out of the firmware image (strings, driver names, the init scripts) and
the live API, not off the PCB. Nothing here required opening the unit.

| Component | Value | How it was established |
|-----------|-------|------------------------|
| SoC | Novatek **NT98566** (internal NA51055) | kernel modules `spi-na51055.ko`, `mmc_na51055`; init prefixed `Nvt`; the board string `DB_566128M5MP_W` (566 = NT98566) |
| CPU | ARM Cortex-A9, ARMv7 EABI5 **hard-float** | busybox is `Version5 EABI, hard-float`, interpreter `/lib/ld-linux-armhf.so.3` |
| Kernel | Linux 4.19.91, preempt | firmware strings |
| Bootloader | U-Boot | `UBOOT_SDRAM_BASE`, `u-boot` in the image |
| NPU | on-chip CNN accelerator | `[ai][kdrv][cnn]`, `arm_ai_snap`, `ai_alarm_human`, and the model blobs under `mnt/src/CNNLib/para/` |
| Image sensor | OmniVision **OS05A10**, 5 MP | init calls `nvt_sen_os05a10`; the firmware filename says `OV05A10` |
| WiFi | Realtek **RTL8812CU / RTL88x2CU**, dual-band USB | driver `rtl88x2CU_WiFi_linux_v5.12.1`, filename `WIFI8812` |
| Storage | one microSD slot | `GetDevInfo diskNum: 1` |
| Audio | one microphone, one speaker, two-way | `GetEnc audio:1`, and the RTSP backchannel |
| Power | the existing doorbell wiring, permanently on | how the unit is installed |

The ARMv7 hard-float line is the one that decides what can run here: anything
new has to be built `arm-linux-gnueabihf`. `reobell` is a Dart AOT build for
that triple, linked against a glibc old enough for the camera's 2.30.

## Flash layout

The `.pak` carries 13 sections onto `/dev/mtd12`. Two of them are filesystems,
each a UBI image holding one UBIFS volume:

| Section | Volume | Mounts at | Stock size | mtd partition |
|---------|--------|-----------|------------|---------------|
| rootfs | `rootfs` | `/` | 20054016 B (153 PEBs) | 32 MB |
| app | `app` | `/mnt/app` | 19529728 B (149 PEBs) | 21 MB |

UBI geometry: PEB 131072, LEB 126976, min I/O 2048, VID offset 2048, lzo
compression. Both filesystems are **read-only**. What persists and is writable
is the `para` mtd at `/mnt/para` (where the baked variant keeps the device key)
and the microSD at `/mnt/sd`.

The exact byte offsets, the header CRC formula and the two version nodes that
have to be bumped together are in
[`../firmware/flashing.md`](../firmware/flashing.md) section 2. They were
measured on the 4662 build and have to be re-measured against any newer one.

## Video and audio

From `GetEnc` and the RTSP SDP, on this unit:

| Stream | Resolution | Codec | Frame rate | Bitrate | GOP |
|--------|-----------|-------|-----------|---------|-----|
| `Preview_01_main` | 2560x1920 | H.264 High | 20 fps | 4096 kbps | 2 |
| `Preview_01_sub` | 640x480 | H.264 High | 10 fps | 256 kbps | 4 |

Audio in is AAC-LC, 16 kHz mono. Audio out, for talking back, is G.711 mu-law
8 kHz mono over the ONVIF RTSP backchannel.

## Detection

- The button press arrives as `visitor.alarm_state` in `GetEvents`. That rising
  edge is what `reobell` turns into a signed XPRS packet.
- Motion is `md.alarm_state`, with an 80x60 zone grid.
- The NPU classifies **people**, **vehicle** and **dog_cat** on this unit;
  `face` and `package` exist in the ability table and are not supported here.

## Ports on the LAN

| Port | Service |
|------|---------|
| 80 | HTTP, the Reolink `api.cgi` JSON API |
| 554 | RTSP (video, AAC audio, mu-law backchannel) |
| 1935 | RTMP |
| 8000 | ONVIF (gSOAP); it publishes a motion topic and no visitor topic |
| 9000 | Baichuan, Reolink's own protocol, also the app's "media port" |

HTTPS exists and is switched off. Logins are session-capped: a handful at once,
then `max session`. The protocol notes, including what the ONVIF surface does
and does not carry, are in [`../docs/device-capabilities.md`](../docs/device-capabilities.md).

## Reolink's published figures, for reference

Not measured here, and listed separately for that reason: roughly 135 degrees
of horizontal field of view, dual-band 2.4/5 GHz WiFi, microSD up to 256 GB,
person and vehicle detection, two-way audio, night vision.

## Getting in without flashing

`serial_check.sh` in the stock rootfs opens a root console on `ttyS0` when the
device name in `/mnt/para/system.cfg` contains `REOCYP`, and the device name is
settable over the ordinary API. That is a root shell with no firmware change,
but it is UART only: the pads are next to the image sensor, 3V3 TTL, 115200
8N1. There is no stock network shell (`telnetd` is in the image, nothing starts
it, and the `inetd.conf` line is commented out), and nothing in the stock
firmware runs a script from the SD card or from `/mnt/para` at boot. That
absence is the whole reason this port reflashes anything at all.
