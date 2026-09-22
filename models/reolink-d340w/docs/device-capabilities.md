> Written while building the desktop companion application for this camera, and
> kept here because it is the protocol map for the device: what answers on which
> port, what the API will and will not tell you, and where its limits are. The
> `tool/...` scripts it names belong to that application and are not part of this
> repository; what runs ON the camera is `../firmware/payload/reobell/`.

# Reolink Video Doorbell WiFi (D340W) — LAN capabilities

Probed 2026-08-24 against 192.168.178.142.

| Field | Value |
|-------|-------|
| model | Reolink Video Doorbell WiFi |
| itemNo | D340W |
| hardVer | DB_566128M5MP_W |
| firmVer | v3.0.0.4662_2508071282 |
| exactType | WIFI_SOLO_IPC, type BELL |
| name | Front door |
| channels | 1 |
| MAC | 94:b3:f7:6c:d9:c9 |

Enabled ports: HTTP 80, RTSP 554, RTMP 1935, ONVIF 8000, Baichuan 9000.
HTTPS disabled.

## 1. Auth — HTTP API

    POST http://<ip>/cgi-bin/api.cgi?cmd=Login
    [{"cmd":"Login","action":0,"param":{"User":{
        "Version":"0","userName":"admin","password":"..."}}}]

Returns `value.Token.name` plus `leaseTime` (seconds). Every later call needs
`?cmd=X&token=<token>`. Renew before the lease expires.

## 2. Ring detection — `GetEvents`

    POST /cgi-bin/api.cgi?cmd=GetEvents&token=<t>
    [{"cmd":"GetEvents","action":0,"param":{"channel":0}}]

Response carries exactly what we need:

    "visitor": { "alarm_state": 0, "support": 1 }      <-- doorbell button
    "md":      { "alarm_state": 0, "support": 1 }      <-- motion
    "ai": { "people": {...}, "vehicle": {...}, "dog_cat": {...} }

`visitor.alarm_state` flips to 1 while the button is pressed. This is the ring
signal. One request returns all of it, so a single poll covers ring + motion +
AI.

## 3. Why not ONVIF events

ONVIF on port 8000 authenticates fine (WS-UsernameToken, PasswordDigest,
SHA1 over nonce+created+password) and `CreatePullPointSubscription` works.
But `GetEventProperties` reports `FixedTopicSet=true` with only:

- `tns1:VideoSource/MotionAlarm`
- `tns1:VideoSource/ImageTooDark/ImagingService`
- `tns1:RuleEngine/CellMotionDetector/Motion`
- `tns1:Media/ProfileChanged`, `tns1:Media/ConfigurationChanged`

**No visitor/doorbell topic.** So ONVIF cannot deliver the button press. Ring
detection has to come from polling `GetEvents`, or from the proprietary
Baichuan push on port 9000.

## 4. Video + audio in — RTSP

    rtsp://<ip>:554/Preview_01_main    2560x1920 h264 High @20fps, 4096 kbps
    rtsp://<ip>:554/Preview_01_sub      640x480  h264 High @10fps,  256 kbps

Both carry audio: `MPEG4-GENERIC/16000`, AAC-hbr, config=1408 (AAC-LC 16 kHz
mono). Digest auth on DESCRIBE.

Use the substream for the instant popup (fast to open, low bandwidth), and
offer a switch to mainstream.

## 5. Talk back — ONVIF RTSP backchannel

This is the important one. Send this header on DESCRIBE:

    Require: www.onvif.org/ver20/backchannel

and the SDP grows a third media section:

    m=audio 0 RTP/AVP 0
    a=control:track3
    a=rtpmap:0 PCMU/8000
    a=sendonly

So two-way audio is reachable with **standard RTSP** — G.711 µ-law, 8 kHz,
mono, sent as RTP payload type 0 on track3. No need to reverse engineer the
proprietary Baichuan protocol on port 9000.

`GetAbility` also reports `talk: {permit:4, ver:1}`.

## 6. Consequences for the app

- Playback: a normal RTSP player can show the stream, but no Flutter player
  package can *send* audio. So the talk path needs our own small Dart RTSP
  client that does DESCRIBE (with the backchannel Require header), SETUP of
  track3 only, PLAY, then streams µ-law RTP from the microphone.
- µ-law encoding is a 16-bit-PCM to 8-bit table lookup, trivial in Dart.
- Discovery cannot rely on WS-Discovery: the doorbell never answered a UDP
  3702 multicast probe, even with ONVIF enabled. Use last-known-IP first,
  then a subnet sweep of port 9000/80 confirmed by an unauthenticated
  `GetDevInfo` returning `rspCode -6`.

## 7. Detection zones

Both detectors take an 80x60 grid, one character per cell, row-major, '1'
watched and '0' ignored.

    GetMdAlarm / SetMdAlarm    MdAlarm.scope.table   motion
    GetAiAlarm / SetAiAlarm    AiAlarm.scope.area    per AI type (people, ...)

Two firmware quirks, both found by probing:

- `SetMdAlarm` answers `param error` (-4) if `newSens` is echoed back, even
  though `GetMdAlarm` returns it. Send `sens` and `useNewSens` without it.
- `scope` must carry `cols` and `rows`; a bare `table` is rejected.

`GetAlarm` is not supported at all on this firmware (`-9`).

`AiAlarm` also carries **`stay_time`**: seconds a target must remain in the
zone before the camera raises the alarm. This is the camera's own dwell
filter, and it separates someone waiting at the door from someone walking
past without any work on our side.

## 8. Session limit

The camera allows only a few concurrent logins. Once they are used up every
`Login` answers:

    {"cmd":"Login","code":1,"error":{"detail":"max session","rspCode":-5}}

and the Reolink phone app is locked out too until the leases expire. There is
no way to list or force-expire sessions, so a leaked token is unusable for the
rest of its lease.

Consequences, both implemented:

- `ReolinkApi.logout()` hands the token back, and is called when the poller is
  torn down, when the app reconnects to a new address, and on window close.
- `tool/probe_camera.py` caches its token in `~/.cache/reolink-doorbell/`
  (mode 600) and reuses it across runs. `--logout` releases it.

If it happens anyway, recover with a token that is still alive:

    python3 tool/probe_camera.py --logout <token>

## 9. No object coordinates

For direction detection, note what the camera does *not* offer. ONVIF
`GetAnalyticsModules` reports one module, a `CellMotionEngine` with a 22x18
cell layout and no object identity or bounding boxes. The metadata
configuration has `Analytics=false`, so no metadata track is streamed, and the
RTSP SDP carries only video and audio. `GetEvents` is booleans only.

So "arriving" versus "leaving" cannot be read from the API as it stands. It
needs either the ONVIF cell-motion metadata stream enabled and its centroid
tracked over time, or local analysis of the substream.

## 10. XPRS LAN bridge (this app → XPRS mesh)

Not a camera feature — a downstream integration. On a doorbell press the app
broadcasts an XPRS message on the LAN so every XPRS station on the subnet
hears it.

- **Reolink firmware is not modified.** It could be (CVE-2025-60855 means the
  D340W accepts unsigned `.pak` images), but that is risky and pointless here:
  the camera has no LoRa/ESP-NOW radio and no XPRS stack, so custom firmware
  would still just be sending to a station over the LAN — which the app does.
- **Transport**: UDP datagram to `255.255.255.255:4242` and each interface's
  directed broadcast (`geogram_xprslan`, `XPRSLAN_PORT`). One datagram = one
  wire packet, verbatim, no framing, no ack.
- **Wire** (`lib/src/xprs/xprs_wire.dart`): `t:message f:X4DOOR
  ts:<utc> scope:local m:Someone at the front door`. `t:` first, `m:` last,
  ≤ 250 bytes, `ts:` UTC. Unsigned (the station accepts unsigned packets).
- **Verified**: after a broadcast a station's own beacon reports
  `hears:X4DOOR`, which the firmware emits only after receiving and parsing the
  packet — so delivery and wire-format are confirmed against live hardware.
- **Gotcha**: `GET /api/xprs/history` is *not* a reliable confirmation on these
  SD-cardless dongles — its row set is effectively frozen and does not reflect
  live LAN traffic. Confirm rendering on the station's own chat screen, not via
  that endpoint.

CLI parity: `tool/xprs_send.py "message" --send`.
