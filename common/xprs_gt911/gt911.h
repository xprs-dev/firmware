/**
 * @file gt911.h
 * @brief GT911 capacitive touch, polled over I2C. One finger is enough.
 *
 * The controller answers at 0x5D or 0x14 depending on the level of its INT
 * pin at power-up, and a board with no RST line to it (the T-Deck) cannot
 * choose -- so gt911_init() probes both and reports which it found.
 *
 * No interrupt: the INT pin is left alone and the status register is polled
 * by whoever owns the I2C bus. That is deliberate twice over -- the GPIO ISR
 * service on the T-Deck belongs to the LoRa radio, and a 30 ms poll is
 * already faster than a finger.
 */
#ifndef GT911_H
#define GT911_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "i2c_bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_dev_handle_t dev;
    uint8_t addr;          /* 0x5D or 0x14, whichever answered */
    uint16_t max_x, max_y; /* the panel's configured output range */
    bool down;             /* last known finger state */
    uint16_t x, y;         /* last known position, raw controller axes */
} gt911_t;

/** Probe the bus. ESP_ERR_NOT_FOUND if neither address answers with a GT911
 *  product id. Call after i2c_bus_init(). */
esp_err_t gt911_init(gt911_t *t);

/** Poll once. Returns true while a finger is down and fills x/y with the
 *  controller's RAW coordinates (the caller applies the board's rotation).
 *  Cheap when nothing changed: one byte on the bus. */
bool gt911_read(gt911_t *t, uint16_t *x, uint16_t *y);

#ifdef __cplusplus
}
#endif

#endif /* GT911_H */
