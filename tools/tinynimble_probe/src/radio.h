/* The five things the probe needs from a BLE stack, so tinynimble and NimBLE
 * can run the SAME test on the SAME board and the comparison means something.
 *
 * Both implementations use identical air parameters, lifted from
 * common/geogram_xprsble/xprsble.c: non-connectable, non-scannable, non-legacy
 * extended advertising, 1M PHY both, sid 0, tx_power 127, 160 ms; extended
 * passive scan 0x0060/0x0050 uncoded with no duplicate filtering. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Called for every XPRS-framed advert heard: the bytes after the marker. */
typedef void (*radio_rx_fn)(const uint8_t *ad, int ad_len, int rssi);

esp_err_t   radio_start(radio_rx_fn cb);
esp_err_t   radio_stop(void);
bool        radio_is_up(void);
esp_err_t   radio_advertise(const uint8_t *ad, int len);
esp_err_t   radio_scan_on(void);
esp_err_t   radio_scan_off(void);
const char *radio_name(void);
