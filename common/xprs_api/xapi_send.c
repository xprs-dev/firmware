/* xapi_send.c -- see the header. No statics live here, deliberately. */
#include "xapi_send.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xprs.h"

void xapi_resp_json(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

esp_err_t xapi_resp_error(httpd_req_t *req, const char *status, const char *why)
{
    xapi_resp_json(req);
    httpd_resp_set_status(req, status);
    char buf[128];          /* short, and the only stack this file spends */
    int n = snprintf(buf, sizeof buf, "{\"ok\":false,\"error\":\"%s\"}", why);
    return httpd_resp_send(req, buf, n);
}

int xapi_jesc(char *out, size_t cap, const char *in, int inlen)
{
    size_t o = 0;
    for (int i = 0; i < inlen && in[i]; i++) {
        unsigned char c = (unsigned char)in[i];
        if (o + 7 >= cap) break;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        /* JSON is UTF-8, and a byte over 0x7f on its own is not valid UTF-8.
         * A wire is text (XPRS.md section 4) but a record recovered from a
         * card need not be, and one stray 0xb7 made the whole reply
         * undecodable -- the same failure the raw control character caused,
         * one layer up. Escaped as its code point, which is valid JSON and
         * keeps the byte visible. */
        else if (c < 0x20 || c > 0x7e)
            o += snprintf(out + o, cap - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
    return (int)o;
}

/* Pull `"key":"value"` out of a JSON body into @p out. False when absent. */
static bool json_str(const char *body, const char *key, char *out, size_t cap)
{
    char k[24];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *j = strstr(body, k);
    if (!j) return false;
    j = strchr(j + strlen(k), '"');
    if (!j) return false;
    j++;
    size_t o = 0;
    while (*j && *j != '"' && o < cap - 1) {
        if (*j == '\\' && j[1]) j++;
        out[o++] = *j++;
    }
    out[o] = 0;
    return true;
}

static bool bearer_known(const char *b)
{
    static const char *const names[] = { XAPI_BEARER_NAMES };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(b, names[i]) == 0) return true;
    return false;
}

esp_err_t xprs_api_send_handler(httpd_req_t *req, char *buf, size_t cap,
                                xapi_send_fn send)
{
    if (!send)
        return xapi_resp_error(req, "404 Not Found", "no transmitter");
    if (!buf || cap < XAPI_SEND_BUF_MIN)
        return xapi_resp_error(req, "500 Internal Server Error", "no scratch");

    /* The caller's one buffer, carved: the body as it arrived, the packet
     * pulled out of it, and the escaped echo. One request at a time on this
     * server, so these never overlap with another handler's use of it. */
    char *body = buf;                     /* 640 */
    char *wire = buf + 640;               /* 256 */
    char *out  = buf + 896;               /* the rest: 1,152 at the minimum */
    const size_t BODY = 640, WIRE = 256, OUT = cap - 896;
    char bearer[12] = "";                 /* "" = every bearer */
    char took[40] = "";

    int total = 0;
    if (req->method == HTTP_GET) {
        /* The captive-WebView fallback: ?wire=<urlencoded packet>. Some
         * sign-in popups cannot POST at all (the old chat page's lesson).
         * A percent-encoded 250-byte packet is 750 characters, so the raw
         * query borrows the answer slice, which is free until the packet
         * has been validated. */
        char *query = out;
        if (httpd_req_get_url_query_str(req, query, OUT) != ESP_OK ||
            httpd_query_key_value(query, "wire", body, BODY) != ESP_OK)
            return xapi_resp_error(req, "400 Bad Request", "need wire=");
        httpd_query_key_value(query, "bearer", bearer, sizeof bearer);
        /* httpd_query_key_value leaves %xx and + in place. */
        char *o = body;
        for (const char *c = body; *c; c++) {
            if (*c == '+') { *o++ = ' '; continue; }
            if (*c == '%' && c[1] && c[2]) {
                char hx[3] = { c[1], c[2], 0 };
                *o++ = (char)strtol(hx, NULL, 16);
                c += 2;
                continue;
            }
            *o++ = *c;
        }
        *o = 0;
        total = (int)(o - body);
    } else {
        while (total < req->content_len && total < (int)BODY - 1) {
            int r = httpd_req_recv(req, body + total, BODY - 1 - total);
            if (r <= 0) return xapi_resp_error(req, "400 Bad Request", "short body");
            total += r;
        }
        body[total] = 0;
    }

    /* Either JSON {"wire":"...","bearer":"..."} or the packet as plain
     * text (then the bearer, if any, came in the query). */
    const char *w = body;
    if (strstr(body, "\"wire\"")) {
        if (!json_str(body, "wire", wire, WIRE))
            return xapi_resp_error(req, "400 Bad Request", "bad json");
        json_str(body, "bearer", bearer, sizeof bearer);
        w = wire;
    }
    if (bearer[0] && !bearer_known(bearer))
        return xapi_resp_error(req, "400 Bad Request", "unknown bearer");
    int wlen = (int)strlen(w);
    while (wlen > 0 && (w[wlen - 1] == '\n' || w[wlen - 1] == '\r')) wlen--;

    /* Validation only, section 4: the caller composed it, the caller owns
     * it -- including the callsign it wrote into f:. */
    if (wlen <= 0 || wlen > XPRS_MAX_WIRE)
        return xapi_resp_error(req, "400 Bad Request", "length");
    if (strncmp(w, "t:", 2) != 0)
        return xapi_resp_error(req, "400 Bad Request", "must start with t:");
    xprs_t pk;
    if (!xprs_parse(w, wlen, &pk))
        return xapi_resp_error(req, "400 Bad Request", "does not parse");
    char from[16];
    if (!xprs_get_str(&pk, "f", from, sizeof from))
        return xapi_resp_error(req, "400 Bad Request", "no f:");

    if (!send(w, wlen, bearer[0] ? bearer : NULL, took, sizeof took))
        return xapi_resp_error(req, "503 Service Unavailable",
                               bearer[0] ? "that bearer did not take it"
                                         : "no bearer took it");

    /* Escaped straight into the answer rather than into a slice of its
     * own: one buffer less, and no reading from the object being written,
     * which is what -Wrestrict objects to and is right to. */
    char id[XPRS_ID_LEN];
    xprs_id(&pk, id);
    int n = snprintf(out, OUT, "{\"ok\":true,\"id\":\"%s\",\"bearers\":\"%s\",\"wire\":\"",
                     id, took);
    if (n < 0 || n >= (int)OUT) return xapi_resp_error(req, "500 Internal Server Error", "no scratch");
    n += xapi_jesc(out + n, OUT - (size_t)n, w, wlen);
    if (n + 3 < (int)OUT) { out[n++] = '"'; out[n++] = '}'; out[n] = 0; }
    xapi_resp_json(req);
    return httpd_resp_send(req, out, n);
}
