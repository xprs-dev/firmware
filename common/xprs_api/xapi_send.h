/*
 * xapi_send -- POST/GET /api/xprs/send, and the three JSON helpers the
 * rest of the API shares with it.
 *
 * WHY THIS IS ITS OWN FILE. The T-Dongle runs its own HTTP server and
 * needed exactly one thing from common/xprs_api: the door that validates a
 * packet somebody composed and hands it to the bearers. Requiring the
 * component pulled in the whole translation unit, and with it every static
 * buffer the other handlers own -- 3,480 bytes of internal DRAM on the one
 * board in the fleet that has fourteen kilobytes free at steady state
 * (docs/esp32.md, "Memory budget"). A linker takes objects, not functions,
 * so the fix is a second object: this one holds no statics at all and the
 * dongle pulls nothing else with it.
 *
 * The buffer comes from the caller for the same reason. Each server already
 * claims one response buffer at boot and shares it across its handlers
 * (docs/esp32.md, "One response buffer, claimed at boot and shared"); a
 * shared door must borrow that, never grow a second one.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

/** Bytes of scratch xprs_api_send_handler() needs. */
#define XAPI_SEND_BUF_MIN 2048

/** application/json + permissive CORS, the same on every endpoint. */
void xapi_resp_json(httpd_req_t *req);

/** `{"ok":false,"error":"..."}` with @p status. Uses no scratch. */
esp_err_t xapi_resp_error(httpd_req_t *req, const char *status, const char *why);

/** JSON-escape @p in into @p out; returns bytes written, excluding the NUL. */
int xapi_jesc(char *out, size_t cap, const char *in, int inlen);

/**
 * The send door. Validates the packet (XPRS.md section 4) and gives it to
 * @p send, which returns true when at least one bearer took it. Answers
 * `{"ok":true,"id":"<derived id>","wire":"<what went out>"}`.
 *
 * @p buf is the caller's response buffer, at least XAPI_SEND_BUF_MIN bytes;
 * this function allocates nothing and keeps nothing.
 */
esp_err_t xprs_api_send_handler(httpd_req_t *req, char *buf, size_t cap,
                                bool (*send)(const char *wire, int len));
