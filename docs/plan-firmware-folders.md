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

## Status (2026-10-25, evening)

Every step is in code and host-tested; the chain is automatic end to end
and asks nobody for anything:

- **Release**: `tools/build-published.sh` builds the images, writes their
  digests into `docs/boards.json`, and runs `tools/scripts/publish_folder.py`,
  which asks the maintainer's own XPRS app (the profile whose key is the
  project's firmware publisher, `common/xprs_ota/fw_publisher.h`) to publish
  the images as the folder: the signed descriptions come back and are
  committed as `docs/firmware.json`. A release without them fails.
- **Every app** (`FirmwareMirrorService`, on by default, six-hourly) reads
  that document, believes only what verifies against the publisher's key,
  downloads and verifies the images, keeps the newest set, serves them by
  digest over Reticulum and Bluetooth, re-airs the signed descriptions
  verbatim and deposits them with its archivers.
- **Every published station** pins the publisher's key on first boot and,
  with `fwauto` on by default, asks its archivers for the publisher's
  folder, fetches a newer image for its board over a Reticulum link and
  resource, and installs it under 11.8 (`common/xprs_fwfetch`).

Released: app 1.3.4 carries the mirror; the beta channel's images
(0.6.0-beta.1) are on `main` under `models/<board>/prebuilt-beta`; stable
stays at the page's 0.5.x images until the beta has proven itself.

**The publishing machine** is set up: the XPRS desktop app on the build
laptop holds the publisher identity (X1WATT, seeded headlessly with the
app's `tool/seed_publisher_profile_test.dart`) with the remote API on, and
the first `docs/firmware.json` carries both channels signed by it. From here
every `tools/build-published.sh` run (stable, or `CHANNEL=beta`) signs and
publishes without anyone touching anything, and the apps pick the result up
within six hours.

**Bench run, 2026-10-25 18:23 to 18:39.** A T-Deck flashed as 0.6.0-beta.0
on `fwchan beta` heard the beta folder's descriptions on the LAN (the
laptop app re-aired them on a mirror tick), resolved X1WATT over
Reticulum, got a 202 to its `cmd:file`, opened the link, streamed the
1,692,480-byte resource through the hub path, installed it and rebooted.
Its next `t:service` announce in the laptop's archive reads
`fw:0.6.0-beta.1 uptime:10m`; the one before the reboot read
`fw:0.6.0-beta.0`. Nobody touched anything. The station exposes no
version over HTTP, so the archive's service announces are the record.

Not written: a station with storage keeping and serving copies to its
neighbours (step 7).

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
