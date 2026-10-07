# T1000-E hardware

The pin map is Meshtastic's (`variants/nrf52840/tracker-t1000-e`), which
runs this board in production; `firmware/src/board.h` is the same table in
code. Nordic pin numbers: P0.n is n, P1.n is 32 + n.

## Radios

| Part | Connection |
|---|---|
| Semtech LR1110 | SPI: SCK P0.11, MISO P1.08, MOSI P1.09, NSS P0.12; RESET P1.10, BUSY P0.07, IRQ (DIO9) P1.01. TCXO on DIO3 at 1.6 V. Transceiver firmware 3.7 on the bench card. |
| RF switch | Driven by the LR1110's DIO5..8, mode by mode (table in `main.cpp`): STBY all low, RX {H,L,L,H}, TX {H,H,L,H}, TX_HP {L,H,L,H}, GNSS {L,L,H,L}. The chip switches them itself, including in duty-cycled receive. |
| nRF52840 Bluetooth | SoftDevice S140 7.3.0, 32 kHz crystal present (LFCLKSRC reads XTAL) |
| Airoha AG3335 | Serial1 at 115200: RX P0.14, TX P0.13. Power P1.11 (high on), reset P1.15 (HIGH resets, LOW runs), VRTC P0.08 (kept high so time and ephemeris survive), SLEEP_INT P1.12 (idle high), RTC_INT P0.15 (idle low), RESETB_OUT P1.14 (input). |

## Sensors and power

| Part | Connection |
|---|---|
| NTC 10 kOhm, B 4250 | AIN7 (P0.31). NTC from the 3.0 V sensor rail to the pin, 8.25 kOhm from the pin to ground. Rail enable P1.06, divider enable P0.04: both are pulsed for a reading and left off. |
| Light | AIN5 (P0.29), same divider enable. Not read yet. |
| QMA6100P accelerometer | I2C SDA P0.26, SCL P0.27, INT P1.02, its own rail on P1.07. Not read yet. |
| Battery | AIN0 (P0.02) through a 1:2 divider, 3.0 V reference, 12 bits |
| Charger | P1.03 low while charging; P0.05 high on external power |

## The person-facing parts

| Part | Connection |
|---|---|
| LED | P0.24, lit high |
| Button | P0.06, active high, pull-down |
| Buzzer | PWM on P0.25, enable P1.05 (held off) |

## Flash

| Range | What |
|---|---|
| 0x00000-0x27000 | MBR and SoftDevice S140 7.3.0 |
| 0x27000-0x67000 | the application (an image is at most 256 KB, the post-build gate stops at 240 KB) |
| 0x67000-0xA7000 | a signed update arriving (`update.cpp`) |
| 0xA7000-0xE7000 | the image that ran before the last update |
| 0xE7000-0xED000 | the mail store, 6 pages (`xprs_mailbox`) |
| 0xED000-0xF4000 | LittleFS: the key and the configuration |
| 0xF4000- | the Adafruit UF2 bootloader (`T1000-E-BOOT`, 239a:8029); its settings page is at 0xFE000 (UICR NRFFW[1]) |

## Holding everything off

The Adafruit core sets every output latch high before `setup()`. The
variant (`variants/seeed_t1000e/variant.cpp`) writes each load's OFF level
before making the pin an output: GNSS power and reset, both sensor rails,
the divider enable, the buzzer, the LED. The variant of the Wio Tracker 1110
must not be used here: it drives P1.01 (the LR1110's IRQ on this card) and
P0.06 (the button).
