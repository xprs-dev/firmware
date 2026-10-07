/*
 * The SenseCAP Card Tracker T1000-E, pin by pin.
 *
 * The variant's map is the identity (variants/seeed_t1000e), so an index here
 * is the Nordic pin: N < 32 is P0.N, N >= 32 is P1.(N-32). Source:
 * Meshtastic's variants/nrf52840/tracker-t1000-e, which runs this board in
 * production; the RF switch table is theirs too (rfswitch.h).
 */
#ifndef T1000E_BOARD_H
#define T1000E_BOARD_H

/* ── LR1110: LoRa transceiver (also GNSS/WiFi scanner, unused here) ───── */
#define T1_LR_SCK       11      /* P0.11 */
#define T1_LR_MISO      40      /* P1.08 */
#define T1_LR_MOSI      41      /* P1.09 */
#define T1_LR_NSS       12      /* P0.12 */
#define T1_LR_RESET     42      /* P1.10 */
#define T1_LR_BUSY      7       /* P0.07 */
#define T1_LR_IRQ       33      /* P1.01, the LR1110's DIO9 */
#define T1_LR_TCXO_V    1.6f    /* TCXO on DIO3 */

/* ── Airoha AG3335 GNSS on Serial1, 115200 ─────────────────────────────── */
#define T1_GNSS_RX      14      /* P0.14 */
#define T1_GNSS_TX      13      /* P0.13 */
#define T1_GNSS_EN      43      /* P1.11, main power, active high */
#define T1_GNSS_RESET   47      /* P1.15, HIGH holds it in reset, LOW runs */
#define T1_GNSS_VRTC    8       /* P0.08, backup domain, kept high */
#define T1_GNSS_SLEEP   44      /* P1.12, idle high */
#define T1_GNSS_RTC_INT 15      /* P0.15, idle low */
#define T1_GNSS_BAUD    115200

/* ── Sensors ──────────────────────────────────────────────────────────── */
#define T1_SENSOR_EN    4       /* P0.04, NTC and light divider */
#define T1_3V3_EN       38      /* P1.06, sensor rail */
#define T1_ACC_EN       39      /* P1.07, accelerometer rail */
#define T1_NTC          31      /* P0.31/AIN7 */
#define T1_LUX          29      /* P0.29/AIN5 */
#define T1_ACC_SDA      26      /* P0.26 */
#define T1_ACC_SCL      27      /* P0.27 */
#define T1_ACC_INT      34      /* P1.02, QMA6100P interrupt */

/* NTC: 10 kOhm at 25 C, B 4250, from the 3.0 V sensor rail to the pin, with
 * 8.25 kOhm from the pin to ground (Meshtastic T1000xSensor.cpp:
 * HEATER_NTC_RP, HEATER_NTC_BX, and its Rt = Rp * Vrail / Vpin - Rp). */
#define T1_NTC_R25      10000.0f
#define T1_NTC_B        4250.0f
#define T1_NTC_RSERIES  8250.0f

/* ── Power ────────────────────────────────────────────────────────────── */
#define T1_VBAT         2       /* P0.02/AIN0, x2 divider, 3.0 V reference */
#define T1_VBAT_MULT    2.0f
#define T1_CHG_DET      35      /* P1.03, LOW while charging */
#define T1_EXT_PWR      5       /* P0.05, external power present */

/* ── Person-facing ────────────────────────────────────────────────────── */
#define T1_LED          24      /* P0.24, lit HIGH */
#define T1_BUTTON       6       /* P0.06, active HIGH, pull-down */
#define T1_BUZZER       25      /* P0.25, PWM */
#define T1_BUZZER_EN    37      /* P1.05 */

#endif /* T1000E_BOARD_H */
