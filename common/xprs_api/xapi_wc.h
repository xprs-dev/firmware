/*
 * xapi_wc -- the httpd glue between xprs_api and the web chat engine
 * (common/xprs_webchat, XPRS.md 10.10): /api/session, /api/admin,
 * /api/rooms, and the hooks /api/services, /api/xprs/history and
 * /api/xprs/send call. The rules live in xwc; this file only moves bytes
 * between the socket and the engine, and keeps the engine's small state on
 * the station's storage.
 */
#ifndef XAPI_WC_H
#define XAPI_WC_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"
#include "xprs_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the engine for this station and register its URIs on [h]. */
void xapi_wc_start(httpd_handle_t h, const xprs_api_cfg_t *cfg);

/** Whether the request's peer is on the station's own network. */
bool xapi_wc_local(httpd_req_t *req);
/** The bytes this answer's body may have (a call made for a visitor on a
 *  Reticulum link by a board with little RAM), 0 when nothing limits it. */
size_t xapi_wc_room(httpd_req_t *req);

/** The `"station":..,"page":{..}` fragment for /api/services (no braces). */
int xapi_wc_services(char *buf, size_t cap);

/** The gate on a wire posted to /api/xprs/send (xwc_gate). */
int xapi_wc_gate(httpd_req_t *req, const char *wire, int len,
                 char id[7], char *why, size_t why_cap);

#ifdef __cplusplus
}
#endif
#endif /* XAPI_WC_H */
