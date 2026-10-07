/*
 * The SenseCAP Card Tracker T1000-E, for the Adafruit nRF52 core.
 *
 * Pins from Meshtastic's tracker-t1000-e variant (variants/nrf52840/
 * tracker-t1000-e/variant.h), which drives this exact board in production.
 * The pin map is the identity: Arduino index N is P0.N for N < 32 and
 * P1.(N-32) above, so every number below reads straight off the schematic.
 *
 * DO NOT BORROW Seeed_Wio_Tracker_1110's initVariant(). It is the nearest
 * stock variant (same nRF52840 + LR1110 family) and it drives P1.01 and
 * P0.06 as outputs: on this board those are the LR1110's IRQ line and the
 * button.
 */
#ifndef _VARIANT_SEEED_T1000E_
#define _VARIANT_SEEED_T1000E_

#define VARIANT_MCK (64000000ul)

#define USE_LFXO                    /* 32.768 kHz crystal (verify: console H) */

#include "WVariant.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PINS_COUNT          (48)
#define NUM_DIGITAL_PINS    (48)
#define NUM_ANALOG_INPUTS   (6)
#define NUM_ANALOG_OUTPUTS  (0)

/* LED: one, green, lit HIGH. */
#define PIN_LED1            (24)        /* P0.24 */
#define LED_BUILTIN         PIN_LED1
#define LED_RED             PIN_LED1
#define LED_BLUE            PIN_LED1
#define LED_STATE_ON        (1)

/* The one button: active HIGH, needs the internal pull-down. */
#define PIN_BUTTON1         (6)         /* P0.06 */

/* I2C: the QMA6100P accelerometer. */
#define WIRE_INTERFACES_COUNT 1
#define PIN_WIRE_SDA        (26)        /* P0.26 */
#define PIN_WIRE_SCL        (27)        /* P0.27 */

/* Serial1: the Airoha AG3335 GNSS, 115200 baud. */
#define PIN_SERIAL1_RX      (14)        /* P0.14 */
#define PIN_SERIAL1_TX      (13)        /* P0.13 */

/* SPI: the LR1110 and nothing else. */
#define SPI_INTERFACES_COUNT 1
#define PIN_SPI_MISO        (40)        /* P1.08 */
#define PIN_SPI_MOSI        (41)        /* P1.09 */
#define PIN_SPI_SCK         (11)        /* P0.11 */
static const uint8_t SS   = 12;         /* P0.12 */

/* Analog references the core's analogRead() expects to find. */
#define PIN_A0              (2)         /* P0.02/AIN0 battery divider */
#define PIN_A1              (31)        /* P0.31/AIN7 NTC */
#define PIN_A2              (29)        /* P0.29/AIN5 light */
#define PIN_A3              (5)         /* P0.05/AIN3 charger detect */
#define PIN_A4              (28)
#define PIN_A5              (30)
static const uint8_t A0 = PIN_A0;
static const uint8_t A1 = PIN_A1;
static const uint8_t A2 = PIN_A2;
static const uint8_t A3 = PIN_A3;
static const uint8_t A4 = PIN_A4;
static const uint8_t A5 = PIN_A5;
#define ADC_RESOLUTION      14

#ifdef __cplusplus
}
#endif

#endif /* _VARIANT_SEEED_T1000E_ */
