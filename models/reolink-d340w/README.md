# Reolink Video Doorbell WiFi (D340W)

A doorbell camera that speaks XPRS for itself. Not a board in the sense the
rest of `models/` means it: there is no ESP32 here and nothing of `common/`
runs on it. It is a small ARM Linux computer sold as a doorbell, and the port
is a userland daemon, `reobell`, baked into a repacked vendor firmware image.

What it does on the network is small and complete. On first boot it generates
a keypair, derives an `X4` callsign from it, and from then on announces a
signed `t:identity` every five minutes and signs and airs a `t:message` the
moment somebody presses the button. Everything goes out as a UDP broadcast on
port 4242, over the WiFi the camera was already associated with. Measured on
the bench, real stations on the LAN reported hearing the camera's own derived
callsign from its own packets, which is the whole point. No desktop application
sits in the middle translating for it, because a device that cannot sign for
itself is a device somebody else is speaking for.

It is an `X4` station in the sense of the specification's section 11.7.1, with
one difference worth stating: 11.7.1 describes a controller holding the
device's private key, and here the device holds its own. A doorbell that runs
Linux does not need anybody to hold its key.

## What is in here

| | |
|---|---|
| Chip | Novatek NT98566 (NA51055), ARM Cortex-A9, Linux 4.19.91 |
| `firmware/` | the whole port: the payload, the repack tooling, the flashing procedure |
| `firmware/payload/reobell/` | what actually runs on the camera, and its Dart source |
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

- **It does not listen.** `reobell` opens no receiving socket. It never
  digipeats, never bridges, never answers a `t:request`, and never appears in
  anybody's `hears:` as a relay. It broadcasts and that is all.
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
- **It is not the XPRS spec's camera yet.** Section 11.7.2 wants a press to be
  `t:observation state:pressed` carrying a `url:` to a still, and `q:snapshot`
  and `q:stream` to be answerable. Today the ring is a `t:message` with prose
  in it, the identity carries an optional `url:`, and answering a request needs
  the listening socket the previous point says it does not have. That is the
  next piece of work on this device, and it is a real gap, not a rounding
  error: a receiver acting on `t:message` is reading a sentence, not a state.

## The camera-side files are editable without reflashing

The deployed variant bakes the payload into the rootfs, which is how it runs
with no SD card in the slot. The other variant flashes a one-line hook and
keeps everything on the microSD, where the config, the poller and the binary
can be edited in place. Both are built by the same tooling and described in
`firmware/flashing.md`; the hook one is the right choice while anything about
the payload is still moving.
