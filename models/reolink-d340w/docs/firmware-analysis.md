# D340W firmware unpack & analysis

Decompression of the stock `.pak` (`firmware/DB_566128M5MP_W.4662_2508071282…`)
and a map of how a future on-camera XPRS daemon could run. Reproduce with
`../firmware/tool/fw_unpack.sh`, which needs the stock image that
`../firmware/stock/fetch-stock.sh` downloads. Nothing here touches the camera.

## Image layout

`.pak` -> 13 sections on `/dev/mtd12` (see `../hardware/HARDWARE.md`). The two filesystem
sections are **UBI images** (`UBI#`), each holding one **UBIFS** volume:

| Section | Container | Volume | Mounts at |
|---------|-----------|--------|-----------|
| `rootfs` | UBI → UBIFS | `rootfs` | `/` (base busybox Linux) |
| `app` | UBI → UBIFS | `app` | `/mnt/app` (Reolink application) |

- UBI: PEB 131072, LEB 126976. Kernel is a U-Boot uImage, Linux-4.19.91/ARM.
- **Both root and app filesystems are read-only** (UBIFS in NAND). Writable,
  persistent areas are the `para` mtd (`/mnt/para`, config) and the microSD
  (`/mnt/sd`, `/mnt/sda`).

## Platform (corrects the earlier SigmaStar guess)

- **SoC: Novatek NT98566** (internal NA51055) — proven by `spi-na51055.ko`,
  `mmc_na51055`, and the `Nvt`/`nvt_info` init. ARMv7, **EABI5 hard-float**
  (busybox interp `/lib/ld-linux-armhf.so.3`, glibc armhf). Kernel 4.19.91.
- Sensor `nvt_sen_os05a10` (OmniVision OS05A10, 5 MP).
- **A future native helper must be built `arm-linux-gnueabihf` (ARMv7,
  hard-float)**, ideally static (musl-armhf) to avoid libc coupling.

## Boot / init flow

`inittab` → `::sysinit:sh /etc/init.d/rcS`. `rcS` runs `/etc/init.d/S[0-9][0-9]*`
in order:

    S00_PreReady  S10_SysInit  S15_NvtAppInit  S25_Net  S99_Sysctl

`S15_NvtAppInit` → `start_app` (in rootfs `/etc/init.d/`) does the real work:
mounts, loads Novatek/AF/Wi-Fi drivers, then from `/mnt/app` launches the
Reolink daemons (`./ftp &`, `./onvif &`, `spawn-fcgi … cgiserver.cgi` on
127.0.0.1:9527, etc.).

**Single best hook point** for any future boot modification: append to
rootfs `/etc/init.d/start_app` (or drop an `S*` script). Both live in the
read-only rootfs, so using them requires reflashing.

## Getting a shell WITHOUT flashing — what's actually possible

- **UART root console (no flash).** `serial_check.sh` sets `/mnt/tmp/serial_open`
  when either a control-GPIO is toggled **or the device name in
  `/mnt/para/system.cfg` contains `REOCYP`**. `start_app` then runs
  `getty 115200 /dev/ttyS0 -l /bin/login`. The device name is settable over the
  normal API, and `/mnt/para` persists — so this flips on a **root serial
  console** with no firmware change. But it is **UART only** (solder/clip to the
  pads near the sensor); it is not network access.
- **No stock network shell.** `telnetd` exists in the image (busybox symlink)
  but nothing starts it, and the `telnet` line in `/etc/inetd.conf` is
  commented out. Once you have the UART root shell you can run `telnetd &` for a
  network shell **for that boot**.
- **No stock "run a script from SD / config".** SD is used only for recordings,
  FTP and HTTP media (`/mnt/sd`, `/mnt/sda`); nothing executes a user script
  from it or from `/mnt/para` at boot. So a dropped script cannot auto-start
  without a hook in the read-only rootfs/app.

**Consequence for "boot a script without modifying installed firmware":** you
can get a **one-shot root shell** with no flash (REOCYP → UART), but making a
custom daemon **auto-start on every boot** has no stock hook — it needs either a
(minimal) reflash of `start_app`, or a not-yet-found command-injection that
executes a value we can persist in `/mnt/para`.

## On-camera XPRS building blocks (for later)

- **Ring detection:** `wget` is present (busybox). Poll
  `http://127.0.0.1/cgi-bin/api.cgi?cmd=GetEvents` locally (needs a `Login`
  token first — api.cgi returns `rspCode -6` unauthenticated). Rising edge of
  `visitor.alarm_state` = the press, exactly like the PC app.
- **UDP broadcast to :4242:** busybox can't set `SO_BROADCAST`, so this needs a
  tiny static `arm-linux-gnueabihf` helper (~20 lines of C) shipped alongside.
- **Config/creds:** the daemon would log in to localhost with the admin
  password — embedding it in a script on the device is the trade-off.

## Toolchain notes (for a later build/repack)

- Unpack: `pakler` + `ubi_reader` (both used by `../firmware/tool/fw_unpack.sh`).
- Repack would need `mkfs.ubifs` + `ubinize` (mtd-utils) with matching UBI
  geometry (PEB 131072, LEB 126976, vol name `rootfs`/`app`), then `pakler -r`
  to re-insert the section and fix the header CRC. New volume must stay within
  its mtd partition (rootfs 32 MB, app 21 MB).
- Recovery if a flash goes wrong: UART pads near the sensor, interrupt U-Boot,
  TFTP the stock `.pak`/sections back.
