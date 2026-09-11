/**
 * @file board.h
 * @brief What a Waveshare ESP32-S3-ePaper-1.54 is: its pins.
 *
 * From Waveshare's own examples (waveshareteam/ESP32-S3-ePaper-1.54,
 * 02_Example/Arduino/07_BATT_PWR_Test/user_config.h and 01_ADC_Test), which
 * agree with common/xprs_model_epaper_1in54/model_config.h, the legacy
 * build's copy of the same map.
 *
 * TWO HARDWARE VERSIONS, ONE PIN MAP. V1 carries an ESP32-S3FH4R2 (4 MB
 * flash, 2 MB quad PSRAM in the package); V2 an ESP32-S3-PICO-1-N8R8 (8 MB
 * and 8 MB). This project is laid out for the smaller one, so it runs on
 * both: the partition table fits 4 MB, and quad PSRAM is what V1 has. The
 * bench board (MAC B8:F8:62:D8:BF:20) reports 4 MB flash, so it is a V1.
 */
#ifndef EPAPER_BOARD_H
#define EPAPER_BOARD_H

#define EPAPER_BOARD_ID "epaper-1in54"

/* The panel: 1.54" 200x200 black and white, an SSD1681-class controller on
 * SPI. Write-only: there is no MISO. */
#define EPD_SPI_HOST   SPI2_HOST
#define EPD_PIN_SCK    12
#define EPD_PIN_MOSI   13
#define EPD_PIN_CS     11
#define EPD_PIN_DC     10
#define EPD_PIN_RST     9
#define EPD_PIN_BUSY    8
#define EPD_W         200
#define EPD_H         200

/* Power switches. The panel and the audio codec each sit behind a switch
 * that is ON when its pin is LOW. VBAT is the other way round and matters
 * more than it looks: on battery the board stays powered only while GPIO17
 * is held HIGH after the PWR button is released. Driving it LOW is how the
 * vendor's firmware switches the board off. */
#define PWR_PIN_EPD     6      /* low = panel powered */
#define PWR_PIN_AUDIO  42      /* low = codec and amplifier powered */
#define PWR_PIN_VBAT   17      /* high = stay on when running from the cell */

/* I2C: the SHTC3 (temperature and humidity, 0x70) and the PCF85063 RTC
 * (0x51), and the ES8311 codec when the audio rail is up. */
#define I2C_PIN_SDA    47
#define I2C_PIN_SCL    48
#define I2C_ADDR_SHTC3 0x70
#define I2C_ADDR_RTC   0x51

/* The cell, through a 1:1 divider (Waveshare: 200K/200K), so the battery is
 * twice what GPIO4 reads. ADC1 channel 3. */
#define BAT_ADC_GPIO    4
#define BAT_DIVIDER     2

/* microSD, SDMMC 1-bit. Not used by this firmware yet. */
#define SD_PIN_CLK     39
#define SD_PIN_CMD     41
#define SD_PIN_D0      40

/* Buttons, both active low. BOOT is the strap pin. */
#define BTN_PIN_BOOT    0
#define BTN_PIN_PWR    18

/* An LED, active low (the vendor's LED test blinks it; the legacy build
 * called it the backlight). Held off. */
#define LED_PIN         3

#endif /* EPAPER_BOARD_H */
