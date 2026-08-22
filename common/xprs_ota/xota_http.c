/**
 * @file xota_http.c
 * @brief POST /api/update -- the one push door, for every board.
 *
 * There used to be two of these, one in xprs_api and one in the T-Dongle's
 * main.c, near-identical and already drifting (one malloc'd, one had a
 * better error message). A firmware door is the last place to keep copies:
 * a fix to the auth check that lands in one and not the other is a station
 * that can be updated by the wrong person. So this is the handler, and a
 * board registers it on whatever HTTP server it runs.
 *
 * Sequence, and why in this order:
 *   1. X-XPRS-Auth, checked BEFORE a byte of body is read -- so a stranger
 *      costs nothing but a header parse. Bound to the approval signature by
 *      `zsha:` (below), so a captured header cannot install something else.
 *   2. X-XPRS-Fw-Version and X-XPRS-Fw-Sig into xota_push_begin, which says
 *      at once whether there is a slot and whether the image fits.
 *   3. The body, streamed in 1 KB pieces into the OTA slot while sha256 runs.
 *   4. xota_push_verify: complete, and the approval signs these exact bytes
 *      for this board and version. This is the answer the pusher gets --
 *      an earlier version of this door replied "installing" BEFORE verifying,
 *      so a refused approval looked like success to whoever pushed it.
 *   5. Only then xota_push_commit, which switches the boot partition and
 *      restarts. The connection dies with the restart; the station says
 *      code:200 over the air once the new image proves itself.
 *
 * THE BODY BINDING. xauth_check_http wants the first 16 hex characters of a
 * sha256 the auth header carries in `zsha:`, so that an authorisation lifted
 * from one request cannot be replayed onto a different body. The body here
 * is 1.4 MB that has not arrived yet when the auth is checked, so it cannot
 * be hashed first. What is bound instead is the APPROVAL -- the X-XPRS-Fw-Sig
 * string -- whose own signature already covers the image's sha256, board,
 * version and size. Authorising an approval is authorising exactly one image.
 * The pusher computes zsha = sha256(X-XPRS-Fw-Sig)[0:16]; tools/push_firmware.sh
 * does.
 *
 * Buffers are static: a board that takes pushes may be one with 14 KB free,
 * and malloc for a header during the one operation that must not fail is
 * the wrong bet.
 */

#include "xprs_ota.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "xprs_auth.h"

static const char *TAG = "xota_http";
static const char *s_call;

static char s_auth[300];
static char s_body[1024];

static esp_err_t reply(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char line[160];
    int n = snprintf(line, sizeof line, "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_send(req, line, n);
    return ESP_OK;
}

static void sha16_of(const char *text, char out[17])
{
    uint8_t d[32];
    mbedtls_sha256((const unsigned char *)text, strlen(text), d, 0);
    static const char *hx = "0123456789abcdef";
    for (int i = 0; i < 8; i++) { out[i * 2] = hx[d[i] >> 4]; out[i * 2 + 1] = hx[d[i] & 15]; }
    out[16] = 0;
}

static esp_err_t h_update(httpd_req_t *req)
{
    char ver[24] = "", sig[80] = "";
    s_auth[0] = 0;
    httpd_req_get_hdr_value_str(req, "X-XPRS-Fw-Version", ver, sizeof ver);
    httpd_req_get_hdr_value_str(req, "X-XPRS-Fw-Sig", sig, sizeof sig);
    httpd_req_get_hdr_value_str(req, "X-XPRS-Auth", s_auth, sizeof s_auth);

    if (!ver[0] || !sig[0])
        return reply(req, "400 Bad Request", "need X-XPRS-Fw-Version and X-XPRS-Fw-Sig");
    if (req->content_len <= 0)
        return reply(req, "400 Bad Request", "no image");

    char zsha[17];
    sha16_of(sig, zsha);
    char who[16] = "";
    xauth_verdict_t v = xauth_check_http(s_auth, s_call, zsha, who);
    if (v != XAUTH_OK) {
        ESP_LOGW(TAG, "update refused: %s",
                 v == XAUTH_403 ? "signer not allowed, or auth not bound to this approval" :
                 v == XAUTH_408 ? "stale, or this station has no clock" : "unsigned");
        return reply(req, v == XAUTH_408 ? "408 Request Timeout" : "403 Forbidden",
                     "this station takes updates only from its owner");
    }

    esp_err_t err = xota_push_begin(ver, (size_t)req->content_len, sig);
    if (err != ESP_OK) {
        /* Say which. "busy or does not fit" used to cover a board with no
         * OTA slot at all, which is the one answer a person cannot act on. */
        const char *why = err == ESP_ERR_NOT_FOUND    ? "this board has no OTA slot (single factory partition)"
                        : err == ESP_ERR_INVALID_SIZE ? "the image does not fit the slot"
                        : err == ESP_ERR_INVALID_STATE? "busy: an update is in progress or awaiting its self-test"
                        : esp_err_to_name(err);
        ESP_LOGW(TAG, "update refused: %s", why);
        return reply(req, "409 Conflict", why);
    }

    int left = req->content_len;
    while (left > 0) {
        int want = left > (int)sizeof s_body ? (int)sizeof s_body : left;
        int got = httpd_req_recv(req, s_body, want);
        esp_err_t wr = got > 0 ? xota_push_write(s_body, (size_t)got) : ESP_OK;
        if (got <= 0 || wr != ESP_OK) {
            ESP_LOGE(TAG, "push stopped after %d of %d: recv=%d write=%s",
                     (int)req->content_len - left, (int)req->content_len,
                     got, esp_err_to_name(wr));
            xota_push_abort();
            return reply(req, "400 Bad Request", "transfer died");
        }
        left -= got;
    }

    err = xota_push_verify();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s pushed %s (%d bytes): refused -- %s", who, ver,
                 (int)req->content_len,
                 err == ESP_ERR_INVALID_CRC ? "approval does not verify for these bytes"
                                            : esp_err_to_name(err));
        return reply(req, "422 Unprocessable Entity",
                     err == ESP_ERR_INVALID_CRC ? "approval does not verify for this image"
                                                : "image rejected");
    }

    ESP_LOGW(TAG, "%s pushed %s (%d bytes): verified, installing", who, ver,
             (int)req->content_len);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"installing\":true}");
    /* The reply is out. Now the restart. */
    err = xota_push_commit();
    if (err != ESP_OK)
        ESP_LOGE(TAG, "commit failed after a verified push: %s", esp_err_to_name(err));
    return ESP_OK;
}

esp_err_t xota_http_register(httpd_handle_t srv, const char *callsign)
{
    if (!srv || !callsign) return ESP_ERR_INVALID_ARG;
    s_call = callsign;
    static const httpd_uri_t u = { .uri = "/api/update", .method = HTTP_POST,
                                   .handler = h_update };
    return httpd_register_uri_handler(srv, &u);
}
