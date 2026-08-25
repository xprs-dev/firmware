#ifndef XPRS_BLE_H
#define XPRS_BLE_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the XPRS BLE service.
 *
 * Initializes NimBLE host/controller and registers the XPRS GATT service.
 */
esp_err_t xprs_ble_init(void);

/**
 * @brief Start BLE advertising for the XPRS service.
 */
esp_err_t xprs_ble_start(void);

/**
 * @brief Stop BLE advertising and shutdown BLE host.
 */
esp_err_t xprs_ble_stop(void);

/**
 * @brief Check whether BLE service is running.
 */
bool xprs_ble_is_running(void);

#ifdef __cplusplus
}
#endif

#endif // XPRS_BLE_H
