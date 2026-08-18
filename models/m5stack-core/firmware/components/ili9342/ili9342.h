/**
 * @file ili9342.h
 * @brief ILI9342C TFT LCD driver for the M5Stack Core (ESP-IDF, SPI).
 *
 * 320x240 landscape, RGB565 big-endian on the wire. Same API shape as the
 * T-Dongle's st7735 driver so a UI layer ports between them mechanically.
 */
#ifndef XPRS_ILI9342_H
#define XPRS_ILI9342_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ILI9342_WIDTH   320
#define ILI9342_HEIGHT  240

typedef struct ili9342_dev *ili9342_handle_t;

typedef struct {
    int mosi_pin;
    int miso_pin;   /* -1 if unused */
    int sclk_pin;
    int cs_pin;
    int dc_pin;
    int rst_pin;    /* -1 if unused */
    int bl_pin;     /* -1 if unused; active-high on the M5Stack Core */
} ili9342_config_t;

esp_err_t ili9342_init(const ili9342_config_t *config, ili9342_handle_t *out);
esp_err_t ili9342_deinit(ili9342_handle_t handle);

/** Push RGB565 pixels (big-endian, caller already byte-swapped) into the
 *  window x1..x2, y1..y2 inclusive. */
esp_err_t ili9342_flush(ili9342_handle_t handle,
                        uint16_t x1, uint16_t y1,
                        uint16_t x2, uint16_t y2,
                        const uint16_t *data);

esp_err_t ili9342_fill_color(ili9342_handle_t handle, uint16_t color);

void ili9342_backlight(ili9342_handle_t handle, bool on);

/** Colour inversion. M5Stack Core panels ship both ways; the default here is
 *  ON (matches current units). Flip if the photo shows negative colours. */
void ili9342_invert(ili9342_handle_t handle, bool on);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_ILI9342_H */
