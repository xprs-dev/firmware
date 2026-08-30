/*
 * The SenseCAP Solar Node P1-Pro, in Arduino pin numbers.
 *
 * THIS FILE EXISTS BECAUSE TWO NUMBERINGS DISAGREE, AND GETTING IT WRONG IS
 * SILENT. Seeed's datasheet names pins the way Nordic does -- P1.13, P0.04 --
 * and the block diagram in ../../hardware/images/block-diagram.png is written
 * that way. The Adafruit nRF52 Arduino core does not take those. Every
 * digitalWrite() and every SPI pin is an INDEX into g_ADigitalPinMap in the
 * Seeed_XIAO_nRF52840_Plus variant, and the core silently ignores an index at
 * or past PINS_COUNT (39) rather than complaining.
 *
 * So each line below carries both, and the mapping was read out of the
 * variant rather than assumed:
 *
 *   framework-arduinoadafruitnrf52/variants/Seeed_XIAO_nRF52840_Plus/
 *
 * DO NOT USE variant.h's OWN NAMES HERE. They describe a bare XIAO, not this
 * carrier board, and several are actively wrong for it: PIN_A0 (index 0) is
 * this board's GNSS wake-up, PIN_NFC1 (index 33) is Grove D1, and LED_RED
 * (index 11) is a LED the P1-Pro does not bring out at all.
 */
#ifndef P1PRO_BOARD_H
#define P1PRO_BOARD_H

/* ── LoRa: Wio-SX1262 on the default SPI bus ─────────────────────────────
 *
 * A convenience worth stating: the variant's default SPI (PIN_SPI_SCK 8,
 * MISO 9, MOSI 10) is exactly P1.13 / P1.14 / P1.15, which is exactly what
 * the datasheet wires the LoRa module to. The `SPI` object is the LoRa bus
 * with no reconfiguration at all. */
#define P1_LORA_SCK    8    /* P1.13 */
#define P1_LORA_MISO   9    /* P1.14 */
#define P1_LORA_MOSI  10    /* P1.15 */
#define P1_LORA_CS     4    /* P0.04 */
#define P1_LORA_DIO1   1    /* P0.03 */
#define P1_LORA_RST    2    /* P0.28 */
#define P1_LORA_BUSY   3    /* P0.29 */

/*
 * The RF switch, and the one thing on this board that is a guess.
 *
 * The datasheet names a GPIO `Lora_sw` and draws it at the module, but does
 * not say what it selects or which way round. Many Wio-SX1262 designs let the
 * chip drive its own switch off DIO2 and expose no such pin at all, and if
 * that is what this is, driving it from here is at best redundant.
 *
 * Held HIGH at boot (see lora_begin) because that is the arrangement most
 * likely to be "RF path enabled", and called out here so that the FIRST
 * thing tried when the radio hears nothing is this line. It costs one
 * digitalWrite to test the other way round.
 */
#define P1_LORA_RF_SW  5    /* P0.05 */

/* ── GNSS: XIAO L76K on Serial1 ──────────────────────────────────────────
 *
 * Also lines up with the variant: PIN_SERIAL1_TX is index 6 (P1.11) and
 * PIN_SERIAL1_RX is index 7 (P1.12), which is what the datasheet labels
 * GNSS_tx and GNSS_rx. Whose TX those names mean is not stated, and the two
 * readings differ by a swap -- so if the receiver is silent with power and
 * reset correct, swap these before suspecting the module.
 *
 * POWER_EN drives a TPS22916 load switch, so the receiver can be cut off
 * rather than merely told to sleep. On a solar node that is the difference
 * that matters, and it is why this firmware leaves the GNSS OFF unless it is
 * asked for. */
#define P1_GNSS_TX        6   /* P1.11 -- Serial1 TX */
#define P1_GNSS_RX        7   /* P1.12 -- Serial1 RX */
#define P1_GNSS_WAKEUP    0   /* P0.02 */
#define P1_GNSS_RST      38   /* P1.03 */
#define P1_GNSS_POWER_EN 37   /* P1.05 -- TPS22916 load switch */

/* ── The two LEDs firmware can actually drive ────────────────────────────
 *
 * There are five LEDs on the board and three of them are wired to the CN3165
 * charger: charging (red), done (green) and solar-present (yellow) are not
 * reachable from here and never will be. A node cannot signal its own
 * battery state with them.
 *
 * That leaves these two, and this firmware spends them the way
 * docs/led-status.md spends an LED elsewhere in the fleet. */
#define P1_LED_USER   30   /* P0.15, white  -- silkscreened USER */
#define P1_LED_MESH   31   /* P0.19, blue   -- silkscreened PWR  */

/* ── Buttons ─────────────────────────────────────────────────────────────
 *
 * Both are inside a sealed outdoor case, which is the whole reason this
 * station is headless: there is no gesture a person can make at it in the
 * field. They are here for the bench.
 *
 * Reset is the nRF52840's own RESET line, not a GPIO, and cannot be read. */
#define P1_BTN_USER   36   /* P1.07 */
#define P1_BTN_POWER  32   /* P1.01 */

/* ── Grove ───────────────────────────────────────────────────────────────
 *
 * Behind an NMOS level shift. P0.09 and P0.10 are the nRF52840's NFC antenna
 * pins in their RESET STATE, and stay that way until CONFIG_NFCT_PINS_AS_GPIOS
 * is written to UICR -- once, permanently, taking effect on the next power
 * cycle. The Adafruit core sets it when a sketch touches these pins, but the
 * pins do not become GPIO until that reboot, so a first bring-up that finds a
 * dead Grove port has almost certainly not power-cycled yet. */
#define P1_GROVE_D1    33  /* P0.09 -- also NFC1 */
#define P1_GROVE_D0    34  /* P0.10 -- also NFC2 */

/* ── Battery ─────────────────────────────────────────────────────────────
 *
 * UNVERIFIED, and deliberately not used. On a bare XIAO nRF52840, VBAT is
 * read on P0.31 behind an enable on P0.14, which is indices 35 and 14 here.
 * The P1-Pro's block diagram shows VBAT reaching the module but names no ADC
 * pin, so whether the carrier keeps that divider is unknown. Reporting a
 * battery percentage from an unverified divider on a solar node is worse than
 * reporting none, so this firmware reports none. Measure it, then use it. */
#define P1_VBAT_ADC     35  /* P0.31 -- unverified on this carrier */
#define P1_VBAT_ENABLE  14  /* P0.14 -- unverified on this carrier */

#endif /* P1PRO_BOARD_H */
