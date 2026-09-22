# D340W firmware build tooling

Reproducible tooling for the modified D340W firmware. Full procedure and the
validated CRC/offset details are in `../flashing.md`.

- `fw_unpack.sh` -- take a stock pak apart (pakler + ubi_reader) into
  `work/unpacked/`, which is how everything in `../../docs/firmware-analysis.md`
  was found out. Reads a file; never touches a camera.
- `Dockerfile` -- the `reobell-mtd` build image: mtd-utils 2.0.2 (source-built,
  matches the camera's Linux 4.19 era), pakler, ubi_reader, python-lzo.
  Build once: `docker build -t reobell-mtd .` from this folder.
- `build_reobell_pak.sh` -- builds the reobell-BAKED pak (firmVer 4664): bakes
  `../payload/reobell/` into the rootfs at `/reobell/`, launches it from
  `start_app`, and REPACKS the pak with a larger rootfs section (app shifted,
  section table + header CRC rewritten). See `../flashing.md` section 7.
- `fw_repack_hook.sh` (+ `_fw_repack_hook_inner.sh`) -- the SD-boot HOOK
  variant: rebuilds only the rootfs with one line appended to `start_app`,
  same geometry as stock, no repack. It runs in a plain `debian:12` container
  and installs what it needs, so it does not use the `reobell-mtd` image.
- `validate_mount.sh <pak>` -- fast pre-flash gate: mounts the pak's rootfs on
  nandsim and checks the reobell launch line. `--privileged`, host modules.
- `validate_update_qemu.sh <pak>` -- authoritative pre-flash gate: runs the
  camera's own `update` flasher on the pak under `qemu-system-arm` and asserts
  `update rootfs success` + `update app success`. See `../flashing.md` section 4.


Always run both validators before flashing a new pak, whichever script built it.

The hook variant (firmVer 4663) is described in `../MODIFYING.md` and
`../flashing.md` sections 1-6; the baked one (4664) in section 7.

The stock image every script starts from is not in git: run
`../stock/fetch-stock.sh` first. Outputs land in `work/` (gitignored). Built
paks carry the real admin password when built with a staged `PAYLOAD`, so they
are kept local, never committed; the
repo `../payload/reobell/config` stays `PASSWORD=CHANGE_ME`.
