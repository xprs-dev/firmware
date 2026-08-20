# LilyGO T-Deck

An XPRS station on a T-Deck: ESP32-S3, a 320x240 ST7789, a trackball, an I2C
QWERTY keyboard, and an SX1262 on 868 MHz -- the station's third bearer,
behind the same relay rules as ESP-NOW and the LAN (`common/geogram_xprslora`).
Validated deck-to-deck on the bench: the same signed packet arriving once by
WiFi and once by RF, the RF copy wearing an RSSI and an SNR.

The station itself is `common/xprs_app`, shared with the M5Stack. What lives
here is the board: `src/board.h` is the pin map, `src/main.c` is the panel, the
trackball and the keyboard, and that is all of it.

```sh
cd firmware
~/.platformio/penv/bin/pio run            # build
~/.platformio/penv/bin/pio run -t upload  # flash
~/.platformio/penv/bin/pio device monitor # 115200
```

## Two S3s on one bench

The T-Dongle is also a native-USB ESP32-S3, so both enumerate as `ttyACM*` in
plug order and flashing the wrong one is easy. `platformio.ini` addresses this
board by its stable by-id path instead:

| Board | Serial | Station |
|---|---|---|
| T-Deck | `DC:DA:0C:3C:24:C8` | this one |
| T-Dongle-S3 | `48:CA:43:4B:B7:C4` | X3WWAJ |

If a flash cannot get the board into download mode, hold the **trackball click**
(GPIO 0 is the strapping pin) while tapping reset.

Sometimes the native-USB console wedges: the port still opens but esptool says
`Could not configure port` or `Write timeout`, and nothing reaches the board.
The JTAG half of the same USB device keeps working, so flash through it --
naming the board's serial, because OpenOCD otherwise takes the first USB-JTAG
device it finds and the T-Dongle is one:

```sh
~/.platformio/penv/bin/pio pkg exec -p tool-openocd-esp32 -- openocd \
  -c "adapter serial DC:DA:0C:3C:24:C8" -f board/esp32s3-builtin.cfg \
  -c "program_esp .pio/build/tdeck/firmware.bin 0x10000 verify reset exit"
```

Only the app moves; the bootloader and partition table do not change between
builds. A `USBDEVFS_RESET` on the device node is worth trying first and is
often enough.

## Controls

The trackball is the M5Stack's three buttons, plus one:

| Gesture | Does |
|---|---|
| click | next panel, or OK on a focused Settings row |
| click, held ~700 ms | back to the home panel |
| roll up / down | move the selection; down on home starts the rotating tour |
| roll left / right | previous / next panel |

A trackball is not a button: rolling it makes and breaks a contact several
times per turn, so `main.c` counts contact CHANGES rather than debouncing a
press, and divides them (`TDECK_TB_DIVIDER`) so one flick is not twenty rows.

While a chat room is open the keyboard writes; Esc (or a long trackball
click) drops the draft and returns to the Radar.

The keyboard feeds the station's console handler, so every key a serial console
understands works from the device: `S` takes a screenshot over serial, `1`-`8`
jump to a panel, `U`/`D` move the selection, `K` is OK, `W` wipes the archive.
Typing text needs a UI that can accept text, which does not exist yet.

## Verifying it

`xui_framedump()` sends the screen over serial as a picture, which is the
honest way to check the UI without believing a log:

```sh
python3 ../../../tools/scripts/framedump.py \
  --port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_DC:DA:0C:3C:24:C8-if00 \
  --cmd S --boot-wait 0 /tmp/tdeck.png
```

`--boot-wait 0` because USB-JTAG does not reset the board when the port is
opened. The M5Stack's CP2104 does, and needs `--boot-wait 45`.

What a framedump cannot tell you is whether the panel itself is right --
colours, orientation, inversion. Only looking at the board does that.

## Measured, first boot

| | T-Deck | M5Stack |
|---|---|---|
| heap before wifi | 179692 (largest 114688) | -- |
| heap after wifi | 125076 (largest 63488) | -- |
| heap after hotspot | 61660 (largest 31744) | ~15700 |
| app partition used | 62% of 2MB | 83% of 1.5MB |

The S3 has room the original ESP32 did not, which is why PSRAM is left off
(see `firmware/sdkconfig.defaults`).

## Not done yet

- **Touch.** The GT911 is on the I2C bus. The eight-panel UI has no touch
  targets, so there is nothing for it to do until there is.
- **The T-Deck Plus** is this board plus a GPS and a battery gauge: a build
  flag, not a project, and `platformio.ini` has the env commented out ready.
  The **T-Deck Pro** is e-paper and shares only the name; it would get its own
  `models/tdeck-pro/`.
