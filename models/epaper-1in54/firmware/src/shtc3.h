/**
 * @file shtc3.h
 * @brief Sensirion SHTC3, temperature and relative humidity over I2C.
 *
 * Small on purpose: one measurement, CRC-checked, and the sensor asleep
 * between readings (it draws under a microamp that way). common/xprs_shtc3
 * does the same job for the legacy build but only compiles when that build's
 * board choice is selected, so this project carries its own.
 */
#ifndef SHTC3_H
#define SHTC3_H

#include <stdint.h>
#include "esp_err.h"
#include "i2c_bsp.h"

/** Wake the sensor and read its ID. ESP_OK means it answered. */
esp_err_t shtc3_probe(i2c_dev_handle_t dev, uint16_t *id);

/** One measurement: degrees Celsius and percent relative humidity, as the
 *  sensor reports them (no self-heating correction). */
esp_err_t shtc3_measure(i2c_dev_handle_t dev, float *temp_c, float *rh_pct);

#endif /* SHTC3_H */
