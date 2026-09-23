# D340W: building and flashing a modified firmware (the reobell hook)

Reproducible, validated procedure to build a modified D340W `.pak` (adds the
reobell SD-boot hook to rootfs) and flash it through the camera's own web UI.
This was validated end to end: the image is accepted by the camera's own
flasher (qemu), and the camera boots it (verified: `firmVer` becomes the bumped
version and streaming keeps working).

Stock (recovery) image: `stock/fetch-stock.sh` downloads it from Reolink and
checks its SHA256; it is not kept in git. Keep the file it leaves behind. To
revert, flash it back (give it a filename with a version number higher than
whatever is currently installed, see section 5).

Every path below is written from the **repository root**, because that is what
the Docker runs mount as `/repo`.

## Cold-start recipe (test on qemu BEFORE flashing)

A fresh session, from a clean checkout, runs this loop. Never flash a pak that
has not passed both validators.

```
# 0. the stock image every build starts from (41 MB, hash-checked)
models/reolink-d340w/firmware/stock/fetch-stock.sh

# 1. build the image
docker build -t reobell-mtd models/reolink-d340w/firmware/tool

# 2. stage the payload with the real admin password OUTSIDE the repo
#    (repo models/reolink-d340w/firmware/payload/reobell/config stays PASSWORD=CHANGE_ME)
cp -a models/reolink-d340w/firmware/payload/reobell ~/reobell_stage
$EDITOR ~/reobell_stage/config          # set the real PASSWORD, BCAST, nick

# 3. build the baked pak (repacks for the bigger rootfs; version 4668+)
docker run --rm --cap-add MKNOD \
  -v "$PWD":/repo -v "$PWD/models/reolink-d340w/firmware/tool/work":/work \
  -e PAYLOAD=/stage -v ~/reobell_stage:/stage -e VER=4669_2509231282 \
  reobell-mtd /repo/models/reolink-d340w/firmware/tool/build_reobell_pak.sh

# 4. VALIDATE on qemu (both must pass; see section 4)
docker run --rm --privileged -v /lib/modules:/lib/modules:ro \
  -v "$PWD":/repo -v "$PWD/models/reolink-d340w/firmware/tool/work":/work \
  reobell-mtd /repo/models/reolink-d340w/firmware/tool/validate_mount.sh /work/reobell_baked.pak
printf 'FROM reobell-mtd\nRUN apt-get update -qq && apt-get install -y -qq qemu-system-arm qemu-user-static\n' | docker build -t reobell-qemu -
docker run --rm --cap-add MKNOD \
  -v "$PWD":/repo -v "$PWD/models/reolink-d340w/firmware/tool/work":/work \
  reobell-qemu /repo/models/reolink-d340w/firmware/tool/validate_update_qemu.sh /work/reobell_baked.pak

# 5. only if both PASS: name it and web-flash from a browser-readable $HOME path
cp models/reolink-d340w/firmware/tool/work/reobell_baked.pak \
   ~/reobell_flash/DB_566128M5MP_W.4669_2509231282.Reolink-Video-Doorbell-WiFi.OV05A10.5MP.WIFI8812.REOLINK.pak
# then upload it in the web UI (section 5). NOT from /tmp: a snap/flatpak
# Chromium cannot read /tmp and uploads an empty file.
```

The built pak carries the admin password, so it stays in `~/reobell_flash/`,
never committed. What "PASS" means and how to read the qemu output is in
section 4. Firmware facts the scripts rely on are in section 2.

## 0. The three things that make a modified pak flash

1. **A rebuilt rootfs UBIFS the camera's Linux 4.19 kernel can mount** -- built
   with `mkfs.ubifs` from mtd-utils 2.0.2 (contemporary with the kernel) and the
   exact stock UBI/UBIFS geometry. Newer mkfs.ubifs from the host also works in
   practice (both produce UBIFS media format w4, same as stock), but 2.0.2 is
   what we validated.
2. **A bumped firmware version, in BOTH places** the camera reads it:
   the uncompressed `version_file` and the lzo-compressed `version.json`
   (`main_ver`, the `app` component `ver`), inside the app UBIFS. The camera's
   local-update version check does `atoi()` on the part before `_`, so the
   PREFIX must increase (4662 -> 4663). We also bump the build-date part.
3. **The correct pak header CRC**, recomputed after every content change (the
   camera's check is a non-standard crc32, see below).

The camera does NOT verify a cryptographic signature. It validates: pak magic
(`0x32725913`), a header type byte, the header CRC, board type, version, and it
must be a mountable UBIFS. All reproducible.

## 1. Build tooling (Docker image `reobell-mtd`)

```
FROM debian:12
RUN apt-get update && apt-get install -y build-essential autoconf automake \
    libtool pkg-config liblzo2-dev zlib1g-dev uuid-dev libssl-dev wget bzip2 \
    ca-certificates python3 python3-pip
RUN pip install --break-system-packages pakler ubi_reader python-lzo
# mtd-utils 2.0.2 from source (matches the camera's kernel era)
RUN cd /tmp && wget -O mtd.tar.bz2 https://infraroot.at/pub/mtd/mtd-utils-2.0.2.tar.bz2 \
    && mkdir mtd && tar xf mtd.tar.bz2 -C mtd --strip-components=1 && cd mtd \
    && (./autogen.sh || autoreconf -fi) && ./configure --without-tests --disable-tests \
    && make -j4 && cp mkfs.ubifs ubinize /usr/local/bin/
```

Run all build steps in this image with `--cap-add MKNOD` (ubireader needs it)
and the repo mounted read-only.

## 2. Pak layout (stock, verified with pakler)

- 13 sections; the two filesystem ones: **rootfs** at byte 2159592, length
  20054016 (153 PEBs); **app** at 22213608, length 19529728 (149 PEBs).
  Sections are contiguous.
- Header CRC = `zlib.crc32(data[1832:] + b"\x02\x00\x00\x00" + section_table[12:844], 0xFFFFFFFF) ^ 0xFFFFFFFF`,
  stored little-endian at offset 4. (Note the double init/xor -- plain
  `zlib.crc32(x)` does NOT reproduce stock's `0xc3febb23`; this variant does.)
- `version_file`: uncompressed UBIFS data node, payload at pak offset 26499728,
  16 bytes ("4662_2508071282\n"); its node starts at -48, node length 64, node
  crc = `crc32(node[8:len]) ^ 0xFFFFFFFF` at node+4.
- `version.json`: lzo-compressed UBIFS data node at app-offset 2346520
  (pak 24560128), node length 285, 237 compressed bytes, uncompressed 781.

## 3. Build steps

Steps 1-4 here are what `tool/fw_repack_hook.sh` automates for the hook
variant; section 7's `tool/build_reobell_pak.sh` does the same and more for the
baked one. Reading them is still worth it, because the two validators can only
tell you that a pak is well formed, not that it is the pak you meant.

1. Split the stock pak (pakler) and extract the rootfs UBIFS **as root** with
   `ubireader_extract_files -k` (keeps the real perms -- the vendor rootfs is
   genuinely 0777/uid-1003, that is correct, not a bug).
2. Append `firmware-hook.sh` to `etc/init.d/start_app` in the extracted tree.
   The hook is boot-loop-safe: it backgrounds, waits up to ~5 min for
   `<sd>/reobell/boot.sh`, execs it if present, else exits. With an empty SD it
   does nothing.
3. Rebuild the rootfs UBIFS + UBI:
   `mkfs.ubifs -m 2048 -e 126976 -c 239 -x lzo -f 8 -k r5 -p 1 -l 4 -r <root> -o r.ubifs`
   then `ubinize -p 131072 -m 2048 -O 2048 -s 2048 -x 1 -Q 153830686 -o rootfs.ubi ini`
   (ini: `[rootfs] mode=ubi image=r.ubifs vol_type=dynamic vol_id=0 vol_name=rootfs vol_alignment=1`;
   ubinize 2.0.2 rejects a `vol_flags` line). Output is exactly 20054016 bytes.
4. Assemble a copy of the stock pak:
   - overwrite the rootfs region (2159592 .. +20054016) with `rootfs.ubi`;
   - bump `version_file` in place: write "4663_2509161282\n", fix its node crc;
   - bump `version.json` in place: lzo-decompress the 237-byte node, replace
     `4662_2508071282` -> `4663_2509161282` (main_ver and the app ver string),
     re-compress with python-lzo level 9, PAD with trailing whitespace so the
     compressed size is exactly 237 bytes again (keeps the node length 285 and
     the UBIFS index intact), update the node's data_size and node crc;
   - recompute the pak header CRC (formula above) and write it at offset 4.

`models/reolink-d340w/firmware/tool/work/` is gitignored; build outputs land there.

## 4. Validate the image on qemu BEFORE flashing (no camera needed)

Always run both checks on the built pak before touching the camera. Two repo
scripts under `tool/` automate them (each takes the pak path):

- **`tool/validate_mount.sh <pak>` -- fast gate: does the rootfs mount?** It
  carves the rootfs section by its section-table entry (so it works for the
  repacked baked pak too), writes it to nandsim, and `mount -t ubifs`. Run it
  `--privileged` with host modules mounted:
  ```
  docker run --rm --privileged -v /lib/modules:/lib/modules:ro \
    -v "$PWD":/repo -v "$PWD/models/reolink-d340w/firmware/tool/work":/work \
    reobell-mtd /repo/models/reolink-d340w/firmware/tool/validate_mount.sh /work/reobell_baked.pak
  ```
  Expect `>>> ROOTFS MOUNTED OK` and `>>> reobell launch line present in
  start_app`. (A missing `/dev/ubi0_0` node reads as `MOUNT FAILED error -22`;
  the script makes the node from sysfs, so a real failure is a genuine UBIFS
  error in dmesg.)

- **`tool/validate_update_qemu.sh <pak>` -- authoritative gate: does the
  camera's OWN flasher accept it?** It extracts the static ARM `update` binary
  from the pak's app fs, fetches the Debian armhf `6.1.0-47-armmp` kernel and its
  matching `nandsim.ko`, builds a busybox initramfs that runs `update 0 /p.pak
  all`, and boots `qemu-system-arm -M virt`. qemu-user cannot do the MTD ioctls,
  so full-system qemu is required. Build the `reobell-qemu` image and run:
  ```
  printf 'FROM reobell-mtd\nRUN apt-get update -qq && apt-get install -y -qq qemu-system-arm qemu-user-static\n' | docker build -t reobell-qemu -
  docker run --rm --cap-add MKNOD \
    -v "$PWD":/repo -v "$PWD/models/reolink-d340w/firmware/tool/work":/work \
    reobell-qemu /repo/models/reolink-d340w/firmware/tool/validate_update_qemu.sh /work/reobell_baked.pak
  ```
  **Pass = both `update rootfs success` and `update app success`, with no
  crc/magic/board/version reject** (the script asserts this and prints `>>>
  PASS`). The `fdt.restore/uenv/sp/ep/NOUSE ... not found in img` lines are
  absent partitions on the single nandsim device and are EXPECTED; only rootfs
  and app matter. The 4665 pak was cleared this way on 2026-09-23, with all six
  sections reporting success.

  **The initramfs busybox must be armhf, and until 2026-09-23 it was not.** The
  script asked apt for `busybox-static` without an architecture inside an amd64
  image, so the initramfs carried an x86-64 binary: the ARM kernel booted, went
  silent at `rdinit=/init` because it could not execute it, and the run died on
  the 240-second timeout reporting "qemu did not reach the update step (boot
  problem or timeout)" -- which reads exactly like a kernel that will not boot.
  It now asks for `busybox-static:armhf` and refuses to boot anything whose ELF
  machine is not ARM. Whether the 4664 pak was ever really cleared by this gate
  is therefore not something this document can claim, and the pak on the camera
  was flashed on the strength of a run that could not have got that far.

  **Caveat (kernel may age out):** `validate_update_qemu.sh` fetches the Debian
  armhf kernel and busybox with `apt download`, so it needs network and the
  configured `KVER` (default `6.1.0-47-armmp`) to still be in the Debian
  archive. If that version is gone, `apt download` fails and the script exits
  with a clear message; override it, e.g. `-e KVER=6.1.0-<newer>-armmp`, and note
  that `nandsim.ko` must come from the SAME kernel deb (the script always takes
  it from the downloaded kernel, so they stay matched). The `-M virt` board and
  the `id_bytes=0x2c,0xda,0x90,0x95` nandsim geometry do not change.

## 5. Flash via the web UI

**Critical gotcha (this cost a lot of time): the browser must be able to READ
the .pak file.** If the file sits somewhere the browser can't read (e.g. under
`/tmp` for a snap/flatpak-confined Chromium), the JS reads 0 bytes and uploads
an empty file; the camera then "Fails" the upgrade with no useful error and
stays on the old firmware. Put the pak under the user's HOME (or any path the
browser can actually open) before selecting it. Verify by reading a slice: the
first bytes must be `13 59 72 32` (the pak magic `0x32725913`).

Steps:
1. Rename the pak to the official pattern with a NEWER version than installed:
   `DB_566128M5MP_W.<VER>.Reolink-Video-Doorbell-WiFi.OV05A10.5MP.WIFI8812.REOLINK.pak`
   (e.g. `<VER>` = `4663_2509161282`). The web UI's `UpgradePrepare` parses the
   filename; a non-matching name -> "Failed to recognize the file format"
   (rspCode -3), the same version -> "identical with the current version" (-30).
2. Camera web UI -> gear -> System -> Maintenance -> Firmware Update -> Browse,
   pick the pak (from a readable location), leave "Reset Configuration"
   UNCHECKED, Update -> Next. It uploads (~1-4 min), writes, and reboots (~60 s).
3. Confirm: `GetDevInfo` `firmVer` becomes `v3.0.0.<VER>` and streaming/snapshot
   still work.

### Flashing without touching a browser window

The upload can be driven headlessly, which is how 4665 and 4666 went on. Start
Chromium with the DevTools port open, log in to the camera page, click through
**Maintenance -> Firmware Update**, hand the file to the input over CDP
(`DOM.setFileInputFiles`, so no native file dialog is involved), then click
**Update** and, on the confirm step that follows, **Next**. The dialog is two
steps: stopping after Update uploads nothing.

```sh
chromium --headless=new --no-sandbox --remote-debugging-port=9222 \
  --user-data-dir=$HOME/reoflash/profile about:blank &
# then drive http://127.0.0.1:9222 : Page.navigate, Runtime.evaluate for the
# login form and the buttons, DOM.setFileInputFiles for the pak.
```

Before clicking Update, read the first four bytes back **through the page**
(`file.slice(0,4).arrayBuffer()`); they must be `13 59 72 32`. That is the
check that catches the empty-upload trap from a browser that cannot read the
path. Watch `document.body.innerText`: `Uploading files...` for a few minutes,
then `Upgrading firmware...`, then the camera drops off the network and comes
back on the login page.

The camera's own `api.cgi` is **not** a way in: `cmd=UpgradePrepare` exists but
answers `rspCode -4 "param error"` to every plausible parameter shape, because
the settings client encrypts its request bodies. Reverse-engineering that is
not worth it when the file input works.

## 6. Prepare the SD card (hook variant only)

Copy `payload/reobell/` to the SD root and edit `reobell/config` (admin
password, nick, broadcast address). On the next boot the flashed hook runs
`<sd>/reobell/boot.sh`: it generates the device key once and starts the signed
XPRS presence beacon + ring poller. See `payload/reobell/README.md`.

## 7. Baking reobell into the rootfs (no SD card) -- the deployed variant

Sections 1-5 build the SD-boot **hook** (firmVer 4663): a stock-shaped rootfs
that only adds one line to `start_app`, then all logic lives on the SD card.
The version actually running on the unit (firmVer **4668**, callsign `X49HRF`)
instead **bakes the whole reobell payload into the rootfs**, so the camera runs
the camera-side XPRS daemon with no SD card. The reobell binary is a 5.7MB Dart
AOT ARM build, which pushes the rootfs to 183 PEBs, past the stock 153-PEB slot,
so this variant also **repacks** the pak with a larger rootfs section.

Build it with `tool/build_reobell_pak.sh` (runs in the same `reobell-mtd`
image). What it does, on top of sections 1-4:

1. **Bake the payload.** Copy `payload/reobell/` into the extracted rootfs
   at `/reobell/` (the binary + `boot.sh` + `config`), `chmod +x` the binary
   and the script.
2. **Launch from `start_app`.** Append one backgrounded line:
   `( sleep 40; /reobell/boot.sh ) >/mnt/tmp/reobell_boot.log 2>&1 &`. The 40s
   delay lets the network and the local `api.cgi` come up first. `boot.sh`
   generates the device key once at **`/mnt/para/reobell.nsec`**. That path is
   not decoration: the rootfs is mounted read-only, so a key written beside the
   binary cannot be created at all, and the daemon then has nothing to sign
   with and says nothing (firmVer 4665 shipped exactly that and was silent on
   the air with port 8080 closed). The suffix is not free either: the camera's
   own `device` binary keeps TLS material in the same directory and clears it
   with `rm /mnt/para/*.crt` and `rm /mnt/para/*.key`, so a key called
   `reobell.key` is deleted between boots and the doorbell greets the LAN as a
   different device every restart (4666 and 4667 did). `/mnt/para` is writable
   and survives a reboot, but **not** a firmware flash: every update gives the
   doorbell a new callsign. `boot.sh` also waits for that mount rather than
   testing it once, because `S00_PreReady` attaches it while the 40s launch
   delay is running. Then it starts the signed presence beacon and supervises
   the daemon.
   (An earlier build also appended a `telnetd -l /bin/sh` debug line; the
   camera's busybox has NO telnetd applet, so that line is inert. It is left out
   of the repo script.)
3. **Rebuild the rootfs UBIFS + UBI** exactly as section 3, but the result is
   now ~184 PEBs (24051712 bytes). It must still fit the 32MB rootfs mtd
   partition (`ROOTFS_PART = 33554432`); the script asserts this.
4. **Repack the pak for the bigger rootfs.** The rootfs and app sections are
   contiguous, so growing the rootfs shifts everything after it:
   - new pak = `stock[:2159592]` + `new_rootfs_ubi` + `app` (version-bumped) +
     `stock[app_end:]` (the trailing sections);
   - rewrite the **section table**: rootfs (section 6) **length** at header
     offset `12 + 6*64 + 60`, app (section 7) **start** at `12 + 7*64 + 56`
     (`start = 2159592 + len(new_rootfs)`). `pakler` reports
     `HEADER_HEADER_SIZE=12`, `SECTION_SIZE=64`, `HEADER_CRC_OFFSET=4`;
   - bump `version_file` and `version.json` in the shifted app bytes (section 3
     step, offsets are relative to the app start so they are unchanged);
   - recompute the pak header CRC (section 2 formula) over the whole new pak.
   The result parses back through `pakler.PAK.from_file` with a valid CRC and is
   ~45.7MB (vs the stock ~41.7MB). Rename it to the official pattern with
   `<VER> = 4669_2509231282` and flash via the web UI (section 5).

   ```
   docker build -t reobell-mtd models/reolink-d340w/firmware/tool
   docker run --rm --cap-add MKNOD \
     -v "$PWD":/repo -v "$PWD/models/reolink-d340w/firmware/tool/work":/work \
     -e VER=4669_2509231282 reobell-mtd \
     /repo/models/reolink-d340w/firmware/tool/build_reobell_pak.sh
   ```

**Password stays out of the repo.** The repo `payload/reobell/config` keeps
`PASSWORD=CHANGE_ME`. To build for a real camera, copy that folder somewhere
OUTSIDE the repo, put the real admin password in its `config`, and point the
script at it with `-e PAYLOAD=/path/to/staged/reobell`. The built pak then
contains the password, so it is kept local (`~/reobell_flash/`), never committed.

Validate the repacked pak the same two ways as section 4 (nandsim mount, and
`update 0 <pak> all` under qemu). Once booted, confirm reobell is live: it
broadcasts a signed `t:identity` on UDP 4242 (any other LAN XPRS station should
report hearing its callsign; after the 4668 flash that is `X49HRF`). The daemon holds one login against the camera's own
api.cgi, watches `GetEvents` for the button and for motion, and airs a signed
`t:observation state:pressed url:http://<ip>:8080/door/snapshot.jpg` (and
`state:motion`, and `state:clear` when the doorstep goes quiet), serving that
url: itself. `curl http://<ip>:8080/api/services` is the quickest check that
it is up.

## 8. Recovery

The rootfs mounts read-only from `ubi0:rootfs`; a bad UBIFS = no boot. The
loader/uboot/kernel are untouched by this mod, so U-Boot stays recoverable over
UART (pads near the sensor, 3V3 TTL, 115200 8N1 on ttyS0; interrupt U-Boot,
TFTP the stock sections). Simplest: re-flash the stock `.pak` from the web UI
(give it a filename version higher than what is installed).
