/**
 * @file epd.h
 * @brief The 1.54" 200x200 e-paper panel (SSD1681 class), refreshed from a
 *        task of its own.
 *
 * A refresh blocks for as long as the panel says it is busy: about 0.3 s for
 * a partial update and 2 s for a full one. None of that may happen on the UI
 * task, which also reads the console and feeds the watchdog, so epd_show()
 * only copies the picture and wakes the panel's task. If a new picture
 * arrives while one is being drawn, the newer one wins and the one in
 * between is never drawn. On this glass a picture nobody saw is not worth
 * two seconds.
 *
 * Full or partial is decided here. The first picture after boot is a full
 * refresh (the panel may be showing anything, including the last picture
 * from before the reset), and so is every EPD_FULL_EVERY'th one after it
 * and any after EPD_FULL_AFTER_S without one, because partial updates leave
 * a faint ghost of what was there before, and a full refresh is what
 * clears it.
 */
#ifndef EPD_DRIVER_H
#define EPD_DRIVER_H

#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"

#define EPD_FULL_EVERY    40
#define EPD_FULL_AFTER_S  3600

typedef struct {
    spi_host_device_t host;
    int sck, mosi, cs, dc, rst, busy;
    int width, height;          /* width must be a multiple of 8 */
} epd_cfg_t;

/** Claim the bus and the pins and start the panel's task. Draws nothing. */
esp_err_t epd_init(const epd_cfg_t *cfg);

/** Queue a picture: one bit per pixel, rows top to bottom, most significant
 *  bit leftmost, 1 = white. Copied; returns at once. Any task. */
void epd_show(const uint8_t *img);

/** Ask for the next picture to be a full refresh. Any task. */
void epd_request_full(void);

/** Refreshes done, for the log and the API. */
uint32_t epd_refresh_count(uint32_t *full);

#endif /* EPD_DRIVER_H */
