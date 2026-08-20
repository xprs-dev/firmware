/**
 * @file board.h
 * @brief What a T-Deck is: its pins, and which T-Deck it is.
 *
 * The pin numbers are LilyGO's own, from Xinyuan-LilyGO/T-Deck
 * examples/UnitTest/utilities.h, and a copy of the schematic belongs in
 * models/tdeck/hardware/ beside them.
 *
 * VARIANTS. The original T-Deck and the T-Deck Plus are the same board with
 * the same pins; the Plus adds a GPS on a UART and a battery gauge on the I2C
 * bus that is already there. That is a build flag, not a project. The T-Deck
 * Pro is an e-paper board with a different panel, a different touch part and a
 * different pin map, and it shares the name and nothing else -- when one
 * arrives it gets models/tdeck-pro/, the way models/epaper-1in54/ already
 * exists for exactly this reason.
 */
#ifndef TDECK_BOARD_H
#define TDECK_BOARD_H

#if defined(TDECK_VARIANT_PLUS)
#  define TDECK_BOARD_ID   "tdeck-plus"
#  define TDECK_HAS_GPS    1
#else
#  define TDECK_BOARD_ID   "tdeck"
#  define TDECK_HAS_GPS    0
#endif

/* The peripheral rail. Everything below -- panel, radio, card, keyboard --
 * is dead until this is driven HIGH, which is why it is the first thing
 * app_main() does and the first thing to suspect when nothing works. */
#define TDECK_POWERON      10

/* One SPI bus, three chip selects. SPI2: on the S3 both buses route through
 * the GPIO matrix, so there is no IOMUX argument for preferring SPI3 the way
 * there is on the original ESP32, and SPI2 is what geogram_sx1262 assumes. */
#define TDECK_SPI_SCLK     40
#define TDECK_SPI_MOSI     41
#define TDECK_SPI_MISO     38
#define TDECK_TFT_CS       12
#define TDECK_TFT_DC       11
#define TDECK_TFT_BL       42
#define TDECK_TFT_RST      (-1)   /* the panel resets with the board */
#define TDECK_SD_CS        39     /* deselect: idle HIGH */
#define TDECK_RADIO_CS      9     /* deselect: idle HIGH */
#define TDECK_RADIO_RST    17
#define TDECK_RADIO_BUSY   13
#define TDECK_RADIO_DIO1   45

/* The keyboard is an ESP32-C3 running LilyGO's firmware; it shares the bus
 * with the touch controller and, on the Plus, the gauge. */
#define TDECK_I2C_SDA      18
#define TDECK_I2C_SCL       8
#define TDECK_KBD_ADDR   0x55
#define TDECK_TOUCH_INT    16

/* The trackball. Four direction pins and a click. LilyGO's own code reads
 * these as levels and acts on a CHANGE rather than on a state -- the ball
 * makes and breaks contact as it rolls, so there is no press to debounce.
 *
 * The click is GPIO 0, which is also the strapping pin: holding the ball
 * down through a reset drops the board into ROM download mode, which is the
 * escape hatch when a flash goes wrong. */
#define TDECK_TB_UP         3
#define TDECK_TB_DOWN      15
#define TDECK_TB_LEFT       1
#define TDECK_TB_RIGHT      2
#define TDECK_TB_CLICK      0

/* One UI step per this many contact changes. The ball is geared finely
 * enough that raw changes scroll a list past reading speed. */
#define TDECK_TB_DIVIDER    2

#endif /* TDECK_BOARD_H */
