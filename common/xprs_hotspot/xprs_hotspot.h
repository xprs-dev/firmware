/**
 * @file xprs_hotspot.h
 * @brief The walk-up hotspot: an open AP whose captive-portal page is a chat.
 *
 * Anyone joining the SSID gets the chat page as their sign-in popup: they
 * make (or import) a NOSTR key that never leaves their browser, get the
 * X3 callsign that key derives, and their messages go out as ordinary
 * signed XPRS packets through the station's own API -- onto ESP-NOW, onto
 * the LAN, into the archive. The station adds no chat machinery at all;
 * the page is a client of spec/API-HTTP.md like any other.
 *
 * Radio reality (docs/esp32.md, and the old firmware's lessons): one radio,
 * so the AP rides the STA's channel when a STA is up (APSTA, driver never
 * stopped, AP DHCP restarted after the mode switch) and ESP-NOW is
 * untouched. Captive detection is served the PAGE (the popup is the chat);
 * every other unknown URI is a 302 plus Connection: close, because captive
 * DNS points every background app at this one socket pool.
 */
#ifndef XPRS_HOTSPOT_H
#define XPRS_HOTSPOT_H

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bring the AP up beside whatever WiFi mode is running, start the wildcard
 * DNS, and register the chat page (/, /generate_204, /hotspot-detect.html
 * and the 404 redirect) on @p server -- the station's shared httpd.
 */
esp_err_t xprs_hotspot_start(const char *ssid, httpd_handle_t server);

/**
 * Register just the chat page (/, the captive-detection URIs and the 404
 * redirect) on @p server, without any AP -- the same page answers on the
 * LAN side. xprs_hotspot_start() calls this itself; calling both is safe.
 */
esp_err_t xprs_hotspot_serve_page(httpd_handle_t server);

/** The AP's netif once xprs_hotspot_start() has made it, else NULL. */
esp_netif_t *xprs_hotspot_netif(void);

/**
 * The AP subnet's directed broadcast (192.168.4.255 by default), network
 * byte order, or 0 before the AP has an address. The LAN bearer airs to it
 * as well, because 255.255.255.255 leaves by the default route, which is the
 * station's own network once it has joined one, and a phone on the hotspot
 * would stop hearing it (XPRS.md 11.10).
 */
uint32_t xprs_hotspot_bcast(void);

/** The access point's own address, network byte order, or 0. */
uint32_t xprs_hotspot_ip(void);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_HOTSPOT_H */
