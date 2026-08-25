/**
 * @file xprs_ui_epaper.h
 * @brief XPRS UI component for e-paper display
 */

#ifndef XPRS_UI_EPAPER_H
#define XPRS_UI_EPAPER_H

#include "esp_err.h"
#include "shtc3.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi connection status
 */
typedef enum {
    UI_WIFI_STATUS_DISCONNECTED = 0,
    UI_WIFI_STATUS_CONNECTING,
    UI_WIFI_STATUS_AP_MODE,
    UI_WIFI_STATUS_CONNECTED,
} ui_wifi_status_t;

/**
 * @brief Initialize the XPRS UI
 *
 * Creates the main screen with temperature, humidity, time and WiFi status.
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t xprs_ui_epaper_init(void);

/**
 * @brief Update temperature and humidity display
 *
 * @param temperature Temperature in Celsius
 * @param humidity Humidity percentage (0-100)
 */
void xprs_ui_epaper_update_sensor(float temperature, float humidity);

/**
 * @brief Update WiFi status display
 *
 * @param status WiFi connection status
 * @param ip_address IP address string (can be NULL if disconnected)
 * @param ssid Connected SSID (can be NULL)
 */
void xprs_ui_epaper_update_wifi(ui_wifi_status_t status, const char *ip_address, const char *ssid);

/**
 * @brief Update time display
 *
 * @param hour Hour (0-23)
 * @param minute Minute (0-59)
 */
void xprs_ui_epaper_update_time(uint8_t hour, uint8_t minute);

/**
 * @brief Update date display
 *
 * @param year Year (e.g., 2024)
 * @param month Month (1-12)
 * @param day Day (1-31)
 */
void xprs_ui_epaper_update_date(uint16_t year, uint8_t month, uint8_t day);

/**
 * @brief Show a status message
 *
 * @param message Status message to display
 */
void xprs_ui_epaper_show_status(const char *message);

/**
 * @brief Trigger a screen refresh
 *
 * @param full_refresh Use full refresh (slower but clears ghosting)
 */
void xprs_ui_epaper_refresh(bool full_refresh);

/**
 * @brief Update uptime display
 *
 * @param uptime_seconds Uptime in seconds
 */
void xprs_ui_epaper_update_uptime(uint32_t uptime_seconds);

#ifdef __cplusplus
}
#endif

#endif // XPRS_UI_EPAPER_H
