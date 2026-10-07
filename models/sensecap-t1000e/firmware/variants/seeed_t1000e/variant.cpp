/*
 * The SenseCAP Card Tracker T1000-E: pin map and the state the board is left
 * in before setup() runs.
 *
 * EVERYTHING OFF. Meshtastic's variant powers the sensors, the accelerometer
 * and the buzzer driver at boot and leaves them on; on a card that has to
 * last three days on 700 mAh that is current nobody asked for. Here each
 * load is switched off before the core ever reaches setup(), and the
 * firmware powers what it uses for as long as it uses it (src/board.h).
 *
 * THE ORDER IS LEVEL FIRST, THEN DIRECTION. The core sets every output latch
 * HIGH before initVariant() (cores/nRF5/wiring.c), so pinMode(OUTPUT) on its
 * own would switch the GNSS, the sensors and the buzzer ON for a moment.
 * digitalWrite(LOW) loads the latch while the pin is still an input.
 */
#include "variant.h"
#include "nrf.h"
#include "wiring_constants.h"
#include "wiring_digital.h"

const uint32_t g_ADigitalPinMap[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
};

static void out(uint32_t pin, uint32_t level)
{
    digitalWrite(pin, level);
    pinMode(pin, OUTPUT);
}

void initVariant()
{
    out(38, LOW);   /* P1.06  3V3_EN: sensor rail */
    out(39, LOW);   /* P1.07  3V3_ACC_EN: accelerometer rail (on when used) */
    out(4,  LOW);   /* P0.04  SENSOR_EN: NTC and light divider */
    out(37, LOW);   /* P1.05  BUZZER_EN */
    out(25, LOW);   /* P0.25  buzzer PWM */
    out(43, LOW);   /* P1.11  GNSS main power */
    out(47, LOW);   /* P1.15  GNSS reset (held low while unpowered) */
    out(8,  HIGH);  /* P0.08  GNSS VRTC: backup domain, keeps hot starts */
    out(44, HIGH);  /* P1.12  GNSS SLEEP_INT, idle high */
    out(15, LOW);   /* P0.15  GNSS RTC_INT, idle low */
    out(24, LOW);   /* P0.24  LED off */
    pinMode(46, INPUT);           /* P1.14  GNSS RESETB_OUT */
    pinMode(6, INPUT_PULLDOWN);   /* P0.06  button, active high */
    pinMode(33, INPUT);           /* P1.01  LR1110 IRQ */
}
