# The stock D340W firmware (not in this repository)

Every build here starts from the vendor's own `.pak` and changes two things
inside it. The stock image is Reolink's, so it is fetched on demand and
verified by hash rather than carried in git:

```sh
./fetch-stock.sh          # downloads here, checks the SHA256, unpacks the zip
```

It lands beside this file as
`DB_566128M5MP_W.4662_2508071282.Reolink-Video-Doorbell-WiFi.OV05A10.5MP.WIFI8812.REOLINK.pak`,
which is the default `STOCK=` for `../tool/build_reobell_pak.sh`. Keep it: it
is also the recovery image. To go back to stock, flash it from the web UI with
a filename carrying a version number higher than whatever is installed (see
`../flashing.md` section 8).

## What this is

The exact build that was running on the unit this model was developed on, and
the latest Reolink publishes for this hardware (`CheckFirmware` reports
`newFirmware: 0`).

| Field | Value |
|-------|-------|
| model | Reolink Video Doorbell WiFi |
| itemNo | D340W |
| hardVer | DB_566128M5MP_W |
| firmVer | v3.0.0.4662_2508071282 |
| detail | S36E1W7T4C0E010A |

## Provenance and integrity

- Reolink download API: product **181** ("Reolink Video Doorbell WiFi",
  hardware `DB_566128M5MP_W`, hwVer id 301), firmware id **833**.
- Direct URL:
  `https://home-cdn.reolink.us/wp-content/uploads/2025/08/201046281755686788.162.zip`
- Cross-checked against the community archive
  [`AT0myks/reolink-fw-archive`](https://github.com/AT0myks/reolink-fw-archive)
  (`firmwares_live.json`, firmware id 833).
- SHA256 of the `.pak`:
  `f1cd81db04fc71f7e3b71908b2d04529bec9b4b25c39bfe907c7f7d3ae60bc52`,
  matching the archive's recorded `sha256_pak`. `fetch-stock.sh` checks it and
  refuses the file if it differs; by hand it is
  `sha256sum DB_566128M5MP_W.4662_2508071282.*.pak`.

## Changelog for this build, as Reolink publishes it

1. Fixed the image overexposure issue.
2. App two-way audio interrupt smart home two-way audio.
3. Updated the photosensitive model.
4. Supplemented and fixed some general bugs.

## A newer stock build

If Reolink ships one, the geometry in `../flashing.md` section 2 (section
offsets, node offsets, the CRC formula) is read off THIS build and has to be
re-measured against the new one with `pakler` before it is trusted. Update the
URL and the hash here in the same change.
