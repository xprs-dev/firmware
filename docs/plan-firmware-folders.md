# Unattended firmware updates over Reticulum, with the format's own folders

*Plan, 2026-10-25. The decision this records: no new mechanism. A firmware
release is a folder published by a person (XPRS.md 7.7.3), found through the
archivers (12.6, 12.9.2), listed by its XFL1 listing (7.7.2), fetched with
`cmd:file` (11.2) over a Reticulum resource, and installed under 11.8's
rules. Every device has Reticulum; a device with storage keeps copies and
serves them; the phones and desktops are holders too.*

## Why the station could not update itself

- It only installs on `cmd:update`; nothing in it ever asks whether there is
  something newer.
- Its configured source is a plain-HTTP feed that does not exist: xprs.dev
  and GitHub both redirect HTTP to HTTPS, and the station has no TLS on
  purpose (11.8: the signature is the trust, and the dongle has no heap).
- Its Reticulum layer is encrypted single packets. No link, no resource, so
  no way to receive a 1.4 MB image the way the format says bytes travel.
- Images are built and committed by hand: no release, no channel, no signed
  descriptions for a reader to find.

## The architecture, piece by piece, and who already has what

| Step | XPRS.md | The app (phone, desktop) | The station (ESP32) |
|---|---|---|---|
| A person publishes a folder: `t:file kind:folder` naming an XFL1 listing | 7.7.3, 7.7.2 | folders exist (op-log lane, DHT, resources); the packet-layer snapshot is **not built** | reads nothing of it yet |
| Each member is described: `t:file file: size: name:` signed | 7.7.1 | not built as a packet | `t:file` is archived (`XI_T_FILE`), not read |
| Descriptions reach readers through archivers | 12.3, 12.6 | `depositArchivers` exists | asks archivers `cmd:history` over Reticulum already (the catch-up poller); needs a second question, `only:<publisher> kind:file` |
| Bytes by hash, `cmd:file` then a Reticulum resource | 11.2, 11.2.2 | **built**: `xprs/files` link, `GET_FILE sha(32)`, the file as one resource, verified by sha | needs a link initiator and a resource receiver in C |
| Holders say what they hold | 8.1, 12.9.2 | `q:have`, provider records | later: `serve:files`, answer `q:have` and `cmd:file` from SD |
| Install, prove, report | 11.8 | pushes exist (`XprsFwPush`) | `xprs_ota` installs a streamed image and reports 200/500 |

## Trust

The folder and every member description are signed by the publisher's key,
the same key a station already pins as `fwkey`. A signed `t:file` whose
`name:` is `<board>-<version>.bin` with `size:` and `file:` is the binding
11.8's approval line makes (board, version, size, digest), as a packet the
format defines. The station accepts an image whose description it verified
against `fwkey` and whose bytes hash to `file:`; nothing else. The `xprsfw1`
approval stays for pushes over a cable or USB.

Two folders per publisher: `XPRS firmware stable` and `XPRS firmware beta`.
A station's `fwchan` picks one; `fwauto` (`off`, `stable`, `beta`) says
whether it installs without being told.

## Status (2026-10-25, night)

Every step is in code and host-tested; the chain is automatic end to end
and asks nobody for anything:

- **Release** is a tag. `git tag v0.6.1 && git push origin v0.6.1` runs
  `.github/workflows/release.yml`: every ESP32 board is built with that
  version and no bench secrets, `tools/scripts/publish_folder.py` signs the
  images as the firmware folder with the publisher key from the repository
  secret `XPRS_PUBLISHER_NSEC` (through `tools/xsign.c`, the station's own
  signer, and refused unless it is the key `common/xprs_ota/fw_publisher.h`
  pins), and the images, `docs/firmware.json`, the catalogue and the page
  are committed to `main` and attached to a GitHub release. A version with
  a `-beta.N` suffix is the beta channel; without, stable. The same chain
  runs on a bench as `tools/build-published.sh`.
- **Every app** (`FirmwareMirrorService`, on by default, six-hourly) reads
  that document, believes only what verifies against the publisher's key,
  downloads and verifies the images, keeps the newest set, serves them by
  digest over Reticulum and Bluetooth, re-airs the signed descriptions
  verbatim and deposits them with its archivers.
- **Every published station** pins the publisher's key on first boot and,
  with `fwauto` on by default, asks its archivers for the publisher's
  folder, fetches a newer image for its board over a Reticulum link and
  resource, and installs it under 11.8 (`common/xprs_fwfetch`). A station
  with a microSD lands the image on the card first (`/idx/fw/<board>-
  <version>.bin`), checks the digest there and only then copies it into the
  OTA slot; the copy stays on the card, the folder's member by its digest,
  for the stations around it (step 7's first half). Written and built; not yet exercised on the bench, whose T-Deck has no card in it at the moment (`/api/status` says `storage: flash`).
- **The ESP32-C3** runs the same fetcher since `v0.6.0-beta.4` (the beta.3
  C3 image has a boot race in the statistics loader and must not be
  flashed). Its receiver is one 6.8 KB block; see below.

**Bench run, 2026-10-25 18:23 to 18:39.** A T-Deck flashed as 0.6.0-beta.0
on `fwchan beta` heard the beta folder's descriptions on the LAN,
resolved X1WATT over Reticulum, got a 202 to its `cmd:file`, opened the
link, streamed the 1,692,480-byte resource through the hub path, installed
it and rebooted. Its next `t:service` announce in the laptop's archive reads
`fw:0.6.0-beta.1 uptime:10m`; the one before the reboot read
`fw:0.6.0-beta.0`. Nobody touched anything. The station exposes no
version over HTTP, so the archive's service announces are the record.

**Second bench run, 2026-10-25 21:12 to 21:27, from a tag.** `v0.6.0-beta.2`
was built, signed and published by the workflow alone. The T-Deck asked
X1WATT, started the resource, lost its hub TCP connection a minute in
(`rns_tcp: recv failed (errno 104)`, reconnected two seconds later): the
link under the transfer dies with that socket, which is what had killed
the two earlier attempts at 1.2 and 1.6 MB. Ninety seconds without a byte,
it opened a fresh link and asked for the rest with `GET_RANGE` from byte
7388, finished at 21:27, installed and restarted as 0.6.0-beta.2. On the
laptop, the station's `cmd:file` had also been meeting the archiver's
stranger budget (two asks an hour, 429): the station now waits ten minutes
after a round that asked and thirty-five after a 429, and an app serves a
mirrored image to anyone past that budget (app commit 836ba63, in the next
app release).

**The ESP32-C3, 2026-10-26.** The same fetcher runs on the board with no
PSRAM and about 30 KB of heap free with the radios up. What had to change
for it was the receiver's memory, not the chain: the resource receiver
kept a 4-byte hash per part for the whole segment (9 KB for a 1 MB
segment, allocated when the advertisement arrived, on a heap whose largest
free block was 13 KB) beside a 7.5 KB part cache. The hashmap is now a
ring of 256 hashes (only the stretch between the frontier and the known
horizon is ever consulted) and the C3 builds with an 8-part cache: one
6.8 KB block for the whole receiver, nothing allocated while the bytes
flow. Its Reticulum task stack goes from 6144 to the 8192 every other
board gives it, because the receiver, the OTA write and a log line now
share that stack. The bench C3 (X3SQS6) was on a 0.1.0 build from before
the fetcher existed, so it got one more flash over the cable; from there
it updates itself like the T-Deck. That flash found two boot-order races
the C3's single core exposes (the statistics loader filling rings
`xst_init` had not allocated, the indexer looking up a Reticulum peer
before `xprsrns_init`), both fixed, and one cadence hole: the C3's first
folder ask went to the T-Deck at the very second the T-Deck was rebooting
into beta.4, and an unanswered ask used to wait six hours. It now retries
in ten minutes, three times, before settling into the long cadence. The
first real attempt then showed the other face of the C3's heap: 16 KB
free the moment the link came up, none of it in one 6.8 KB piece, so the
receiver's calloc failed and the image was held against for six hours.
The receiver is now a 2 KB block plus part slots of 472 bytes taken one
at a time, and an attempt that ends for a reason of the station's own (no
memory, the hub socket gone) is deferred ten minutes, not failed. The
attempt after that found the order wrong: the slot was opened at the
link's proof, opening it quiesces the station (hotspot down, storage
paused), and on the C3 that bounced the hub TCP socket one second later,
under a link that had just come up: the GET_FILE went into a dead socket.
The fetcher now opens the slot on the holder's 202, settles five seconds,
and only then asks for the link.

Not written: a station serving the copies on its card to its neighbours
(`serve:files`, `q:have`, `cmd:file` as a responder: step 7's second half).

## Work, in order

1. **App core, `XprsFolder`**: build and parse XFL1; publish a folder
   snapshot (listing stored content-addressed and held, `t:file kind:folder`,
   one `t:file` per member) and deposit it with the archivers. Pure, tested.
2. **Station, `xprs_folder`**: parse XFL1 and member descriptions, verify
   against `fwkey`, pick the newest image for this board and channel. Pure C,
   host-tested.
3. **Station, discovery**: the archiver poller asks `only:<publisher>
   kind:file` on its own cadence; replayed descriptions feed step 2.
4. **Station, Reticulum link and resource receiver** in C, wire-compatible
   with RNS 1.3.5 and checked against vectors reticulum-dart produces, parts
   streamed to a sink (the OTA slot, or a file on SD). This is the large
   piece and the one that makes "use Reticulum" true.
5. **Station, the fetch and install**: `cmd:file` to a holder, the resource
   into `xprs_ota`, verify, install, report 200/500 to the owner and the
   archivers; `fwauto` policy, idle only, jittered, never the same failed
   digest twice.
6. **App, the publisher's folder**: the always-on station with the publisher
   key fills the stable and beta folders from the firmware releases (sha256
   in the catalogue so it verifies before holding) and publishes a new
   snapshot when a release lands.
7. **Station with storage keeps and serves**: hold the folder's members on
   SD, announce `serve:files`, answer `q:have` and `cmd:file` (responder link,
   resource sender). After this a T-Deck feeds the stations around it with no
   phone present.

Steps 1 and 2 are independent of each other; 3 to 5 depend on 2 and 4; 6
depends on 1; 7 depends on 4.
