# LilyGO T-Deck

An XPRS station on a T-Deck: ESP32-S3, a 320x240 ST7789, a trackball, an I2C
QWERTY keyboard, and an SX1262 on 868 MHz -- the station's third bearer,
behind the same relay rules as ESP-NOW and the LAN (`common/xprs_bearer_lora`).
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

And the glass is a touch panel (GT911, on the keyboard's I2C bus):

| Touch | Does |
|---|---|
| tap the bottom bar | what the slot says: **Home**, **Prev**/**Next**, **OK** on Settings |
| swipe left / right | next / previous panel |
| tap a table row | select it; on Settings a second tap on the selected row is OK |
| drag a table | scrolls it, and it stays where you left it |
| tap a room (chat) | open it; tap the composer to put the caret back |
| any touch or key while the screen is dark | wakes it, and does nothing else |

Changing a setting takes **Enter**, not a trackball click. A click is far too
easy to make by accident while rolling to a row, and it was silently flipping
radios; the bottom bar names the key that actually acts. The ball is also rate
limited (`TDECK_TB_MIN_GAP_MS`), so one flick moves one row however fast it
spins.

The bottom bar names what a tap does because this board has no buttons
under it; a board with buttons (the M5Stack) keeps its button legends.

**Keyboard backlight** lights on any keypress and goes out after 5 s idle.
**Screen** blanks after `screen_off_s` (config.ini, default 120, 0 = never)
seconds idle -- but only once the battery trend says it is discharging, so
a station on the bench stays lit. Both panel and backlight sleep; the radios
and LVGL's touch polling do not.

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

## Updating over the air

Two app slots and rollback, like the other boards -- this one used to have a
single `factory` partition and could only be reflashed by cable. The push
door is the shared one (`docs/device.md` §6); `tools/push_firmware.sh` is the
procedure. The archive gave up 2 MB for the second slot and moved, so the
first boot on the new table reformatted it once; NVS did not move and the
station kept its identity.

## The boot splash

The XPRS triad, drawn as strokes rather than stored as a picture: the ten
shapes in `spec/artwork/splash/xprs-triad-dark.svg` are a coordinate table
in `common/xprs_art/` (212 bytes of `.rodata`) and ten `lv_line` objects, the
same idiom the radar's rings use. A 320x240 RGB565 bitmap would have been
153,600 bytes and useless on the T-Dongle's 160x80 panel.

It carries a line naming what is starting -- `network`, `services`, `mesh`,
`identity`, `radio`, `ready` -- and goes as soon as the dashboard behind it
has been filled in once.

**The panel now comes up before WiFi rather than after everything.** It used
to be last, which was defensible ("everything it reads already exists by
then") but meant the glass stayed dark through the whole slow part and then
showed a finished dashboard. Two things fall out of the move: the splash
covers seconds that used to be blank, and the draw buffer gets the contiguous
DMA block it wants -- **30 rows instead of 15**, so every frame afterwards
flushes in half as many SPI slices. It is deliberately **not** moved ahead of
BLE, whose controller needs internal DRAM this would take.

## Not done yet

- **Battery state** is inferred from the voltage TREND (six samples a minute
  apart, ±15 mV), not from a charger pin -- the T-Deck has none. It cannot
  tell "full on USB" from "full and just unplugged" for the first minute.
- **Keyboard modifiers.** Shift and Alt are the keyboard MCU's: it sends the
  shifted or symbol byte. Nothing on the S3 side interprets them.
- **The T-Deck Plus** is this board plus a GPS and a battery gauge: a build
  flag, not a project, and `platformio.ini` has the env commented out ready.
  The **T-Deck Pro** is e-paper and shares only the name; it would get its own
  `models/tdeck-pro/`.
