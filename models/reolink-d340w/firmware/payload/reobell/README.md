# reobell - camera-side XPRS bridge (runs on the doorbell, 24/7)

Files that live on the **SD card** at `<sdcard>/reobell/`. The one-time firmware
hook (`../../firmware-hook.sh`, baked into `start_app`) runs `boot.sh` from here at
every boot, so everything below is editable **without reflashing**.

The doorbell is a proper XPRS **X4 device**: it signs what it says and announces
itself, so any XPRS station on the LAN learns it and verifies its messages. No
desktop app is involved.

| File | Role |
|------|------|
| `boot.sh` | launched by the firmware hook; generates the key once, runs the beacon + poller |
| `xprsbell.sh` | polls local `api.cgi` for the button, calls `reobell` to sign+broadcast on a ring |
| `reobell` | Dart binary (ARMv7 hard-float): signs XPRS packets and UDP-broadcasts to :4242 |
| `reobell.key` | the device private key (nsec), created on first boot; **never in the repo** |
| `config` | admin password, message, nick, optional url, broadcast addr |
| `src/` | the `reobell` Dart source, to rebuild the binary |

## What it broadcasts

- **Presence**, every 5 minutes: a signed
  `t:identity f:X4<derived> k:npub1... nick:frontdoor [url:...] ts:... scope:local sig:...`
- **Ring**: on the button, a signed
  `t:message f:X4<derived> ts:... scope:local sig:... m:Someone at the front door`

The `X4` callsign is derived from the device key; the signature is the XPRS
short-Schnorr (`sig:`, 60 base85 chars) that every XPRS station verifies. Tested
live: real stations on the LAN reported `hears:X4<derived>` after these packets.

## Install

1. Edit `config`: set `PASSWORD` to the camera's admin password. Optionally set
   `NICK`, `BCAST` (e.g. `192.168.178.255`), and `URL`.
2. Copy the whole `reobell/` folder (including the `reobell` binary) to the root
   of the camera's microSD card.
3. Reboot the camera (once the hooked firmware is flashed). On first boot it
   generates `reobell.key` and prints the callsign to `/mnt/tmp/reobell_boot.log`.

## Rebuilding the `reobell` binary

Pure Dart, cross-compiled from an x64 host with the Dart SDK (>= 3.10):

    cd src
    dart pub get
    dart compile exe --target-os linux --target-arch arm -o ../reobell bin/reobell.dart

The result is ARMv7 EABI5 hard-float, links glibc <= 2.17 (the camera has 2.30),
and uses `/lib/ld-linux-armhf.so.3` - it runs on the doorbell unmodified. The
crypto is vendored from the XPRS reference (`XprsCrypto`, `NostrCrypto`) so
signatures stay byte-identical to the network. `dart run bin/reobell.dart
selftest` checks a sign/verify round-trip.

## Notes

- The admin password and the device key sit on the SD card; neither leaves the
  camera and neither is committed to the repo.
- Logs: `/mnt/tmp/reobell.log` and `/mnt/tmp/reobell_boot.log`.
- `xprsbcast` / `xprsbcast.c` are the old unsigned v1 broadcaster, superseded by
  `reobell` (which also broadcasts). Kept only for reference.
