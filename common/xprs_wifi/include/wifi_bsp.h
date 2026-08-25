#ifndef WIFI_BSP_H
#define WIFI_BSP_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi connection status
 */
typedef enum {
    XPRS_WIFI_STATUS_DISCONNECTED = 0,
    XPRS_WIFI_STATUS_CONNECTING,
    XPRS_WIFI_STATUS_CONNECTED,
    XPRS_WIFI_STATUS_GOT_IP,
    XPRS_WIFI_STATUS_AP_STARTED,
    XPRS_WIFI_STATUS_AP_STACONNECTED,
    XPRS_WIFI_STATUS_ERROR,
} xprs_wifi_status_t;

/**
 * @brief WiFi event callback type
 */
typedef void (*xprs_wifi_event_cb_t)(xprs_wifi_status_t status, void *event_data);

/**
 * @brief WiFi STA configuration structure
 */
typedef struct {
    char ssid[32];
    char password[64];
    xprs_wifi_event_cb_t callback;
} xprs_wifi_config_t;

/**
 * @brief WiFi AP configuration structure
 */
typedef struct {
    char ssid[32];
    char password[64];
    uint8_t channel;
    uint8_t max_connections;
    xprs_wifi_event_cb_t callback;
} xprs_wifi_ap_config_t;

/**
 * @brief Initialize WiFi subsystem
 *
 * @return esp_err_t ESP_OK on success
 */
/**
 * @brief Stop the automatic reconnect while this station is deliberately off
 *        the access point's channel (XPRS.md §23.7).
 *
 * Leaving the channel is indistinguishable from losing the link, so without
 * this the reconnect logic drags the station home in under a second and the
 * meeting never happens. Always paired: hold before moving, release on return.
 */
void xprs_wifi_hold_reconnect(bool hold);

esp_err_t xprs_wifi_init(void);

/**
 * @brief Deinitialize WiFi subsystem
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t xprs_wifi_deinit(void);

/**
 * @brief Connect to WiFi network as station
 *
 * @param config WiFi station configuration
 * @return esp_err_t ESP_OK on success
 */
esp_err_t xprs_wifi_connect(const xprs_wifi_config_t *config);

/**
 * @brief Disconnect from WiFi network
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t xprs_wifi_disconnect(void);

/**
 * @brief Start WiFi access point
 *
 * @param config WiFi AP configuration
 * @return esp_err_t ESP_OK on success
 */
esp_err_t xprs_wifi_start_ap(const xprs_wifi_ap_config_t *config);

/**
 * @brief Stop WiFi access point
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t xprs_wifi_stop_ap(void);

/**
 * @brief Drop the SoftAP but KEEP the STA connection (APSTA -> STA-only).
 *
 * Unlike xprs_wifi_stop_ap() (which esp_wifi_stop()s everything), this only
 * switches to STA-only mode and stops the AP DHCP server, leaving the STA
 * associated. Used once the iGate is on the LAN to free the radio (the SoftAP is
 * a third radio user that destabilises WiFi/BLE coexistence).
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t xprs_wifi_disable_ap_keep_sta(void);

/**
 * @brief Check if AP mode is active
 *
 * @return true if AP is running
 */
bool xprs_wifi_is_ap_active(void);

/**
 * @brief Get current WiFi status
 *
 * @return xprs_wifi_status_t Current status
 */
xprs_wifi_status_t xprs_wifi_get_status(void);

/**
 * @brief Get current IP address (STA mode)
 *
 * @param ip_str Buffer to store IP string (at least 16 bytes)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t xprs_wifi_get_ip(char *ip_str);

/**
 * @brief Get AP IP address
 *
 * @param ip_str Buffer to store IP string (at least 16 bytes)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t xprs_wifi_get_ap_ip(char *ip_str);

/**
 * @brief Get AP IP address as uint32_t
 *
 * @param ip_addr Pointer to store IP address
 * @return esp_err_t ESP_OK on success
 */
esp_err_t xprs_wifi_get_ap_ip_addr(uint32_t *ip_addr);

/**
 * @brief Load saved WiFi credentials from NVS
 *
 * @param ssid Buffer for SSID (at least 33 bytes)
 * @param password Buffer for password (at least 65 bytes)
 * @return esp_err_t ESP_OK if credentials found
 */
esp_err_t xprs_wifi_load_credentials(char *ssid, char *password);

/**
 * @brief Connect STA while keeping AP running (AP+STA mode)
 *
 * Properly restarts the WiFi driver to switch from AP to AP+STA mode.
 * The event handler will call esp_wifi_connect() on STA_START.
 *
 * @param ssid Network SSID
 * @param password Network password (NULL for open networks)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t xprs_wifi_connect_sta(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif

#endif // WIFI_BSP_H
