/**
 * @file st7789.h
 * @brief ST7789 TFT LCD driver (ESP-IDF, SPI).
 *
 * Same API shape as the M5Stack's ili9342.h and the T-Dongle's st7735.h, so a
 * UI layer ports between them mechanically -- that is the whole point of the
 * three of them looking alike.
 *
 * Two things this one has that those do not, both because the T-Deck puts the
 * panel, the SD card and the SX1262 on ONE SPI bus:
 *
 *   - `host` is a parameter, not a constant. Whoever else is on the bus may
 *     have initialised it first.
 *   - `clock_hz` is a parameter. Through the ESP32-S3's GPIO matrix a panel
 *     that is happy at 40 MHz on a dedicated bus may not be, and bring-up is
 *     not the time to find out.
 *
 * The panel is natively 240x320 portrait; MADCTL rotates it to 320x240
 * landscape, which is what ST7789_WIDTH/HEIGHT report.
 */
#ifndef XPRS_ST7789_H
#define XPRS_ST7789_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST7789_WIDTH   320
#define ST7789_HEIGHT  240

typedef struct st7789_dev *st7789_handle_t;

typedef struct {
    spi_host_device_t host;   /* SPI2_HOST etc. -- the bus may be shared */
    int mosi_pin;
    int miso_pin;   /* -1 if unused */
    int sclk_pin;
    int cs_pin;
    int dc_pin;
    int rst_pin;    /* -1 when the panel resets with the board (T-Deck) */
    int bl_pin;     /* -1 if unused; active-high */
    int clock_hz;   /* 0 selects a conservative 20 MHz */
} st7789_config_t;

esp_err_t st7789_init(const st7789_config_t *config, st7789_handle_t *out);
esp_err_t st7789_deinit(st7789_handle_t handle);

/** Push RGB565 pixels (big-endian, caller already byte-swapped) into the
 *  window x1..x2, y1..y2 inclusive. */
esp_err_t st7789_flush(st7789_handle_t handle,
                       uint16_t x1, uint16_t y1,
                       uint16_t x2, uint16_t y2,
                       const uint16_t *data);

esp_err_t st7789_fill_color(st7789_handle_t handle, uint16_t color);

void st7789_backlight(st7789_handle_t handle, bool on);

/** Put the panel to sleep (DISPOFF + SLPIN) or wake it (SLPOUT, 120 ms,
 *  DISPON). The backlight is separate: turn it off too, or the sleeping
 *  panel glows white. Waking redraws nothing by itself -- the caller
 *  invalidates the screen. */
void st7789_sleep(st7789_handle_t handle, bool sleep);

/** Colour inversion. ST7789 panels are almost always wired so that this
 *  belongs ON, which is the default; flip it if a photograph of the board
 *  shows a negative image. */
void st7789_invert(st7789_handle_t handle, bool on);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_ST7789_H */
