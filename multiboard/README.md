# The shared multi-target build

One PlatformIO project, eight boards. They share `src/main.cpp` and the whole of
`common/`; what makes each of them itself is an sdkconfig fragment and a
`xprs_model_*` component. That is why they are one project rather than eight:
splitting them would mean eight copies of a `main.cpp` that is currently one.

```sh
~/.platformio/penv/bin/pio run -e tdongle_s3
```

Each board's own folder under `../models/` holds its sdkconfig, its
documentation and its hardware assets. This directory holds only what they
share.

## Where things point

Three paths here reach outside this directory, and all three were repaired when
the tree was reorganised:

| | |
|---|---|
| `components` | a symlink to `../common`, so the eighty-odd `-Icomponents/...` flags in `platformio.ini` keep resolving unchanged |
| `../models/<board>/sdkconfig.<board>` | each env's `SDKCONFIG_DEFAULTS`; the board's config lives with the board |
| `scripts/pre_build.py`, `post_build.py` | this project's own PlatformIO hooks, and nothing else's |

## Build status, honestly

Not all eight compile today, and **they did not compile before the move
either**. Each was built in the old `aurora/esp32` tree and here, and the set of
errors compared:

| Target | Status |
|---|---|
| `tdongle_s3` | **builds** |
| `esp32c3_mini` | fails: `msgstore.h`, `nimble/nimble_port.h` -- identical before and after |
| `heltec_v3` | fails: `nimble/nimble_port.h` -- identical before and after |
| `esp32_generic` | fails at link -- identical before and after |
| `heltec_v1`, `heltec_v2`, `kv4p`, `esp32s3_epaper_1in54` | not retested |

So the reorganisation is faithful, including where it is faithful to breakage.
Those failures are a real debt -- components pulling in NimBLE and the message
store on boards whose configuration does not enable them -- but fixing them is
its own job, not part of moving files.

The T-Dongle-S3 target here is the **legacy** BLE APRS build. What actually
ships for that board is `../models/tdongle-s3/firmware/`, which has its own
project. Both are kept deliberately.
