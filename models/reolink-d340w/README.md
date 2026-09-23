# Reolink Video Doorbell WiFi (D340W)

A doorbell camera that speaks XPRS for itself. Not a board in the sense the
rest of `models/` means it: there is no ESP32 here and nothing of `common/`
runs on it. It is a small ARM Linux computer sold as a doorbell, and the port
is a userland daemon, `reobell`, baked into a repacked vendor firmware image.

What it does on the network is small and complete. On first boot it generates
a keypair, derives an `X4` callsign from it, and from then on says what the
door does the way the format says a device says it (XPRS.md 11.7, 11.7.2):

```
t:identity    f:X4... k:npub1... nick:frontdoor url:http://<ip>:8080/door/snapshot.jpg
t:observation f:X4... state:pressed url:http://<ip>:8080/door/snapshot.jpg
t:observation f:X4... state:motion  url:http://<ip>:8080/door/snapshot.jpg
t:observation f:X4... state:clear
```

and serves that `url:` itself. Everything goes out as a UDP broadcast on port
4242, over the WiFi the camera was already associated with. Measured on the
bench, real stations on the LAN reported hearing the camera's own derived
callsign from its own packets, which is the whole point. No desktop application
sits in the middle translating for it, because a device that cannot sign for
itself is a device somebody else is speaking for.

**The unit is a version behind this tree.** It runs firmVer 4664, whose press
is a `t:message` -- chat, which put a bubble with a Reply button in the Local
room on every ring -- and which carries no `url:`. The daemon described here
says it as an observation instead and is built and bench-tested; it reaches the
camera at the next flash.

It is an `X4` station in the sense of the specification's section 11.7.1, with
one difference worth stating: 11.7.1 describes a controller holding the
device's private key, and here the device holds its own. A doorbell that runs
Linux does not need anybody to hold its key.

## What is in here

| | |
|---|---|
| Chip | Novatek NT98566 (NA51055), ARM Cortex-A9, Linux 4.19.91 |
| `firmware/` | the whole port: the payload, the repack tooling, the flashing procedure |
| `firmware/payload/reobell/` | the daemon that runs on the camera, and its Dart source |
| `firmware/stock/` | how to fetch and verify the vendor image every build starts from |
| `hardware/` | what the camera is made of, and how it was found out |
| `docs/` | the stock firmware taken apart, and the camera's LAN protocols |

## Building it

There is no `pio run` here. The image is the vendor's own `.pak` with a rebuilt
rootfs inside it, and it is built in Docker because it needs `mkfs.ubifs` from
the camera's kernel era:

```sh
cd firmware
stock/fetch-stock.sh                 # the vendor image, verified by SHA256
docker build -t reobell-mtd tool
docker run --rm --cap-add MKNOD \
  -v "$PWD/../../..":/repo -v "$PWD/tool/work":/work \
  -e VER=4664_2509161282 reobell-mtd \
  /repo/models/reolink-d340w/firmware/tool/build_reobell_pak.sh
```

Then validate it twice and flash it through the camera's web UI. The full
procedure, the pak geometry and the CRC formula are in
[`firmware/flashing.md`](firmware/flashing.md); `firmware/README.md` is the
shorter map of the two build variants.

**Never flash a pak that has not passed both validators in `firmware/tool/`.**
The rootfs mounts read-only from NAND, so a bad UBIFS is a camera that does not
boot, and the way back is a UART clip onto pads next to the image sensor.

## What this device cannot do

- **It does not listen on the air.** `reobell` opens no XPRS socket: it never
  digipeats, never bridges, and never appears in anybody's `hears:` as a relay.
  It broadcasts, and separately serves HTTP on 8080 for the picture its
  observations point at, which carries no XPRS packet at all.
- **It carries no XPRS radio bearer.** No BLE, no LoRa, no ESP-NOW. The only
  way off this device is the house WiFi it is already on, which means it is
  reachable exactly as far as the LAN reaches and no further. A station on the
  same LAN is what carries its packets onto the air.
- **Its firmware path is unsigned, and that cuts both ways.** The camera
  accepts any `.pak` with a correct header CRC, the right board type and a
  higher version number (CVE-2025-60855). That is what makes this port
  possible; it is also why the device should be treated as something an
  attacker on the LAN could reflash, and why its admin password never belongs
  in this repository. The built pak carries that password, so built paks stay
  local and are never committed.
- **It cannot be asked anything.** Section 11.7.2 also wants `q:snapshot`,
  `q:stream` and `q:state` answered, and that needs a listening socket this
  daemon does not open. What it says, it says unprompted. A station wanting a
  picture reads the `url:` off the last observation instead, which is the same
  address it would have been given.

## The camera-side files are editable without reflashing

The deployed variant bakes the payload into the rootfs, which is how it runs
with no SD card in the slot. The other variant flashes a one-line hook and
keeps everything on the microSD, where the config and the binary can be
replaced in place. Both are built by the same tooling and described in
`firmware/flashing.md`; the hook one is the right choice while anything about
the payload is still moving.
