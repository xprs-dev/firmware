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

/**
 * The cable. `cfg get <key>`, `cfg set <key> <value>`, `cfg del <key>` on
 * any board's serial console, so a pinned key, an owner or a setting can be
 * changed with a USB lead and nothing else -- which is what the docs
 * promised ("re-writable with a cable") and what no board actually had.
 * Returns true when @p line was a cfg command (handled, answer printed).
 * One implementation; every console calls it first.
 */
bool xcfg_console(const char *line);

/** Render the whole configuration as config.ini text. The stored nsec is
 *  never printed -- the file shows a blank slot to write one into. */
int xcfg_ini_render(char *buf, size_t cap);

/** Parse config.ini text and persist every recognised key. */
esp_err_t xcfg_ini_apply(const char *text, size_t len);

/**
 * Start/stop the browser editor (HTTP, port 80). Serves GET / (editor
 * page), GET /config.ini (raw) and POST /config.ini (apply + restart).
 *
 * NO AUTHENTICATION AND NO TLS. Anybody who can reach port 80 on this
 * station can read its configuration -- WiFi password slot aside, the nsec
 * is never rendered -- and can write a new one and restart it. That is a
 * deliberate trade for a device configured on a home LAN with a cable's
 * worth of trust, and it is exactly why an always-on station is NOT
 * port-forwarded.
 *
 * A super-archiver does not need to be. It DIALS OUT to a hub ([rns] hub)
 * and is reachable through the connection it opened; there is no inbound
 * listener for XPRS traffic here at all. Anyone reasoning about "how do I
 * reach my archiver from outside" should reach for that, never for a port
 * forward to this editor.
 */
/** Tell the editor where the rotating log lives; GET /log.txt then streams
 *  the previous file followed by the current one. Call before or after
 *  start; NULL paths disable it. */
void xcfg_share_set_log(const char *current, const char *previous);

/** Register the share's URIs on an existing server instead of owning one.
 *  Call before xcfg_share_start(). With no attach, start() runs its own
 *  server on port 80 as before. */
void xcfg_share_attach(void *httpd_handle);

esp_err_t xcfg_share_start(void);
void      xcfg_share_stop(void);
bool      xcfg_share_running(void);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_CONFIG_H */
