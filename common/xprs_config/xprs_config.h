/**
 * @file xprs_config.h
 * @brief Station configuration: NVS-backed settings, a config.ini face, and
 *        an opt-in browser editor.
 *
 * The station's editable settings live in NVS and wear a config.ini face for
 * humans. Boards without native USB (the classic ESP32 has none -- its USB
 * socket is a serial bridge) share the file over HTTP instead: flip the
 * share on and the device serves an editor at http://<ip>/ where anyone can
 * change the WiFi, the station name, or paste an nsec to give the station an
 * identity -- no keyboard on the device needed. Boards with native USB
 * (S2/S3) can expose the very same file as a removable drive later; the
 * format is the contract.
 *
 * Known keys (INI section_key -> NVS key):
 *   [station] name -> "name", nsec -> "nsec" (write-only: consumed at boot)
 *   [wifi]    enabled -> "wifi_on", ssid -> "ssid", password -> "pass"
 *   [espnow]  enabled -> "espnow_on"
 *   [share]   enabled -> "share_on"
 */
#ifndef XPRS_CONFIG_H
#define XPRS_CONFIG_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Load the config cache from NVS. Call once, before anything reads it. */
esp_err_t xcfg_init(void);

/** Get a value; returns def when the key was never set (never NULL if def
 *  is not NULL). The pointer stays valid until the key is next set. */
const char *xcfg_get(const char *key, const char *def);

/** "yes"/"no", "true"/"false", "1"/"0" all work. */
bool xcfg_get_bool(const char *key, bool def);

/** Set and persist one value. An empty string erases the key. */
esp_err_t xcfg_set(const char *key, const char *value);

esp_err_t xcfg_set_bool(const char *key, bool value);

/** Render the whole configuration as config.ini text. The stored nsec is
 *  never printed -- the file shows a blank slot to write one into. */
int xcfg_ini_render(char *buf, size_t cap);

/** Parse config.ini text and persist every recognised key. */
esp_err_t xcfg_ini_apply(const char *text, size_t len);

/** Start/stop the browser editor (HTTP, port 80). Serves GET / (editor
 *  page), GET /config.ini (raw) and POST /config.ini (apply + restart). */
esp_err_t xcfg_share_start(void);
void      xcfg_share_stop(void);
bool      xcfg_share_running(void);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_CONFIG_H */
