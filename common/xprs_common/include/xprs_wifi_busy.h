/**
 * @file xprs_wifi_busy.h
 * @brief "WiFi is serving somebody": one signal for the radios that share it.
 *
 * Whoever serves a person over WiFi says so for a few seconds at a time: the
 * web server on each connection (xprs_api), the Reticulum responder while a
 * visitor's link is open and on each request (xprsrns). Whoever shares the
 * radio reads it and steps back for that long: Bluetooth steps back
 * (xprs_app, xprsble_step_back) and ESP-NOW does not leave the WiFi channel
 * (xprsnow). docs/esp32.md, "One radio".
 *
 * No task, no lock: two words written and read whole.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** WiFi serves somebody for at least the next [ms]. */
void     xprs_wifi_busy(uint32_t ms);
/** True until the last xprs_wifi_busy() runs out. */
bool     xprs_wifi_is_busy(void);
/** Milliseconds since the last xprs_wifi_busy(), or UINT32_MAX if never. */
uint32_t xprs_wifi_busy_since_ms(void);

#ifdef __cplusplus
}
#endif
