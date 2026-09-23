# reobell - the XPRS daemon that runs on the doorbell, 24/7

What sits on the camera. In the deployed build it lives in the rootfs at
`/reobell/`; in the SD-boot variant it is `<sdcard>/reobell/`, where it can be
edited without reflashing.

The doorbell is a proper XPRS **X4 device**: it holds its own key, signs what
it says, and announces itself, so any XPRS station on the LAN learns it and
verifies its reports. No desktop application sits in the middle.

| File | Role |
|---|---|
| `boot.sh` | launched by the firmware hook; makes the key once, then supervises the daemon |
| `reobell` | the daemon: one ARMv7 hard-float binary, built from `src/` |
| `config` | admin password, nick, the port, the broadcast address, the debounces |
| `reobell.key` | the device private key (an nsec), made on first boot; **never in the repo** |
| `src/` | the Dart source, to rebuild the binary |
| `xprsbcast.c` | the first unsigned broadcaster, from before any of this. Reference only. |

## What it does

One process, because the camera counts sessions:

- **Holds one login** against the camera's own `api.cgi`, over loopback, and
  renews it at four fifths of its lease. The D340W allows only a few at once,
  cannot be made to forget one, and locks the household out of the Reolink app
  when they run out. A refused login backs off to a minute rather than
  retrying every second.
- **Watches the door** with `GetEvents`, which carries the button and motion in
  one answer, and airs what it sees the way XPRS.md 11.7 says a device says it:

```
t:observation f:X4... state:pressed url:http://<ip>:8080/door/snapshot.jpg ts:... sig:...
t:observation f:X4... state:motion  url:http://<ip>:8080/door/snapshot.jpg ts:... sig:...
t:observation f:X4... state:clear                                          ts:... sig:...
```

  A press is aired every time it happens. Both it and movement are debounced
  (5 s and 30 s), which answers "is this the same press, the same person
  walking past" and never "is this worth anybody's attention": that second
  question belongs to whoever hears it. `clear` goes out once, after the
  doorstep has been quiet for twenty seconds.
- **Announces itself** every five minutes with a signed `t:identity` carrying
  its npub, its nick and the same `url:`. The address is worked out again each
  time, because a DHCP lease moves.
- **Serves the picture** those URLs point at, on port 8080 (80 is the camera's
  own web UI):

| | |
|---|---|
| `GET /door/snapshot.jpg` | a current still, `image/jpeg` |
| `GET /door/stream.mjpeg` | `multipart/x-mixed-replace`, about 1.5 frames a second |
| `GET /api/services` | what it serves, so a client asks once instead of probing |

  It proxies: the daemon calls `Snap` itself with the token it already holds
  and streams the bytes back, so **no credential is ever in a URL it hands
  out** (11.7.2 says so about RTSP passwords; a session token is no different).
  One camera call serves every viewer inside a second, so ten phones and a
  stream loop are one `Snap`, not eleven.

  The stream is honestly a run of stills. The camera serves h264 over RTSP and
  an ARMv7 doorbell cannot transcode, so `multipart/x-mixed-replace` is what it
  can offer a browser and 1.5 fps is what that costs.
- **Airs nothing that is chat.** A doorbell does not belong in a conversation:
  a station's chat ring admits `t:message` and `t:status`, and a phone's
  `#LOCAL` room admits any undirected `scope:local` message without caring
  whether a person or a machine sent it. So a press was a bubble with a Reply
  button on it, in among people talking. The format's answer is the type: what
  a thing has to say is an observation, and what a receiver does about one is
  the receiver's business.

## Install

1. Edit `config`: the camera's admin password, and anything else you want
   different. Quote any value with a space in it.
2. Copy the whole `reobell/` folder (binary included) to the SD card root, or
   let `tool/build_reobell_pak.sh` bake it into the image.
3. Reboot the camera. On first boot it makes `reobell.key` and prints the
   callsign to `/mnt/tmp/reobell_boot.log`.

## Rebuilding the binary

Pure Dart, cross-compiled from an x64 host with the Dart SDK (>= 3.10):

```sh
cd src
dart pub get
dart compile exe --target-os linux --target-arch arm -o ../reobell bin/reobell.dart
dart run bin/reobell.dart selftest     # sign/verify round trip
```

ARMv7 EABI5 hard-float, `/lib/ld-linux-armhf.so.3`, glibc old enough for the
camera's 2.30. The crypto is vendored from the XPRS reference so signatures
stay byte-identical to what the network verifies.

## Running it anywhere but the camera

Everything comes from the environment, so the daemon can be pointed at a
stand-in while it is worked on:

```sh
REOBELL_KEY=/tmp/bell.key REOBELL_API=http://127.0.0.1:8098 \
REOBELL_USER=admin REOBELL_PASS=sesame REOBELL_HTTP_PORT=8097 \
dart run bin/reobell.dart run 127.0.0.1
```

`REOBELL_POLL_MS`, `REOBELL_MOTION_DEBOUNCE_S`, `REOBELL_CLEAR_AFTER_S`,
`REOBELL_IDENTITY_S` and `REOBELL_URL` are the rest.

## Notes

- The admin password and the device key sit beside each other on the camera.
  Neither leaves it: the password is only ever sent to loopback, and the key
  only ever signs.
- Logs: `/mnt/tmp/reobell.log` and `/mnt/tmp/reobell_boot.log`.
- What it does not do yet: answer `q:snapshot`, `q:stream` or `q:state`
  (XPRS.md 8, 11.7.2). That needs a listening socket, which is a bigger change
  than airing what it already knows.
