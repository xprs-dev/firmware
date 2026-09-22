# The D340W firmware port

What builds, validates and flashes the XPRS firmware for the Reolink doorbell.
This is not a PlatformIO project: the output is the vendor's own `.pak` with a
rebuilt rootfs inside it, and the camera's own updater is the flasher.

```
firmware/
  payload/reobell/   what runs on the camera: the daemon, its scripts, its Dart source
  tool/              the Docker build image, the pak builder, the two validators
  stock/             fetch and verify the vendor image every build starts from
  firmware-hook.sh   the one-line boot hook, for the SD-card variant
  flashing.md        the full procedure: geometry, CRC, validation, web UI, recovery
  MODIFYING.md       the hook variant on its own, and what it changes in the rootfs
```

## Two variants, one payload

| | Baked (deployed) | Hook |
|---|---|---|
| firmVer | 4664_2509161282 | 4663_2509161282 |
| Where the payload lives | inside the rootfs at `/reobell/` | on the microSD at `<sd>/reobell/` |
| Needs an SD card | no | yes |
| Changing the payload | reflash | edit the card |
| Pak is repacked | yes, the rootfs outgrows its 153-PEB slot | no, same geometry as stock |

The baked one is what runs on the unit. The hook one is the better one to be
holding while the payload is still changing, because everything after the flash
is an edit on a card rather than another firmware build.

## Build

```sh
stock/fetch-stock.sh                 # 41 MB from Reolink's CDN, SHA256 checked
docker build -t reobell-mtd tool
docker run --rm --cap-add MKNOD \
  -v "$PWD/../../..":/repo -v "$PWD/tool/work":/work \
  -e VER=4664_2509161282 reobell-mtd \
  /repo/models/reolink-d340w/firmware/tool/build_reobell_pak.sh
```

The build writes `tool/work/reobell_baked.pak`, which is gitignored, as is
everything else in `tool/work/`.

To build for a real camera, the payload needs the camera's admin password, and
the repository copy must not have it. Stage the payload outside the tree, put
the password in its `config`, and point the builder at it:

```sh
cp -a payload/reobell ~/reobell_stage
$EDITOR ~/reobell_stage/config       # PASSWORD, NICK, BCAST
docker run --rm --cap-add MKNOD -e PAYLOAD=/stage -v ~/reobell_stage:/stage \
  -v "$PWD/../../..":/repo -v "$PWD/tool/work":/work \
  -e VER=4664_2509161282 reobell-mtd \
  /repo/models/reolink-d340w/firmware/tool/build_reobell_pak.sh
```

The pak built that way carries the password. It stays local. `payload/reobell/config`
in this repository keeps `PASSWORD=CHANGE_ME` and the builder warns when it sees it.

## Validate, then flash

Two gates, both in `tool/`, both taking the pak path, and **both** have to pass
before anything reaches the camera:

- `validate_mount.sh` mounts the pak's rootfs on nandsim and checks that the
  reobell launch line is in `start_app`. Fast.
- `validate_update_qemu.sh` runs the camera's **own** `update` binary over the
  pak under `qemu-system-arm` and asserts `update rootfs success` and
  `update app success`. Authoritative: it is the same code that will run on the
  device.

Then rename the pak to the vendor's filename pattern with a higher version
number and upload it in the camera's web UI. `flashing.md` has the exact
commands, what the qemu output means, and the recovery path when a flash goes
wrong. The trap that costs the most time is in there too: a confined browser
cannot read a file under `/tmp`, uploads nothing, and the camera reports a
failure that says nothing useful.

## Rebuilding the payload binary

`payload/reobell/reobell` is a Dart AOT build for ARMv7 hard-float, committed
because the build needs a Dart SDK and the pak build does not:

```sh
cd payload/reobell/src
dart pub get
dart compile exe --target-os linux --target-arch arm -o ../reobell bin/reobell.dart
dart run bin/reobell.dart selftest     # sign/verify round trip
```

The crypto is vendored from the XPRS reference so the signatures stay
byte-identical to what every other station verifies.
