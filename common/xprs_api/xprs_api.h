/**
 * @file xprs_api.h
 * @brief The station's HTTP API (spec/API-HTTP.md), board-agnostic.
 *
 * One httpd instance on port 80, worker pinned to core 1 (docs/esp32.md:
 * every handler may touch the flash and httpd has ONE worker task). The
 * board supplies what only it knows -- its callsign, its index, how to air
 * a wire, what its switches say -- through the config struct; everything
 * about HTTP lives here and is identical on every board that starts it.
 *
 * Other modules (the config share) register their extra URIs on the same
 * server via xprs_api_httpd().
 */
#ifndef XPRS_API_H
#define XPRS_API_H

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xprsidx_s;   /* geogram_xprsindex; NULL when the board has no store */

typedef struct {
    const char *app;              /* "xprs-esp32" */
    const char *board;            /* "m5stack-core", "tdongle-s3", ... */
    const char *callsign;
    struct xprsidx_s *index;      /* may be NULL */

    /** Air one validated wire on the board's bearers. Cheap: called on the
     *  httpd task. Return true when at least one bearer took it. */
    bool (*send_wire)(const char *wire, int len);

    /** JSON fragments the board owns, written into @p buf: the serve list
     *  (e.g. `"index","history","mailbox"` -- no brackets) and the feature
     *  object body (e.g. `"digipeater":false,...` -- no braces). Either may
     *  be NULL. Return bytes written. */
    int (*serve_json)(char *buf, size_t cap);
    int (*features_json)(char *buf, size_t cap);

    /** Extra members for /api/status, e.g. `"battery":{...}` -- written
     *  AFTER a leading comma is emitted, so just the members. NULL for
     *  nothing extra. Must not block: it runs on the HTTP task. */
    int (*status_json)(char *buf, size_t cap);

    /** The rotating log files (newest lines at the END of cur). NULL when
     *  the board keeps no log. */
    const char *log_cur, *log_prev;

    /** The timezone string for /api/status, e.g. "+00:00". */
    const char *tz;
} xprs_api_cfg_t;

/** Start the server. The cfg (and everything it points at) must outlive it. */
esp_err_t xprs_api_start(const xprs_api_cfg_t *cfg);

/** POST/GET /api/xprs/send, for a board that runs its own httpd: validate
 *  the wire (section 4) and hand it to @p send. Returns the id it will
 *  have on the air. The only door through which a signed command can be
 *  put on the radio from the bench. */
esp_err_t xprs_api_send_handler(httpd_req_t *req,
                                bool (*send)(const char *wire, int len));

/** The shared server handle, for modules adding their own URIs.
 *  NULL before xprs_api_start(). */
httpd_handle_t xprs_api_httpd(void);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_API_H */
