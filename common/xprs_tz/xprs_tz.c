/*
 * The station's time zone, asked of the internet. See xprs_tz.h.
 *
 * Plain HTTP on a raw socket rather than esp_http_client: the client
 * registers its TLS transport whenever HTTPS support is configured, and
 * the boards trimmed mbedTLS's TLS half out on purpose (docs/esp32.md). A
 * GET, a few hundred bytes of JSON and three keys do not need a library.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xprs_tz.h"

/* ── JSON, the three shapes these answers use ──────────────────────────── */

/* The value after "key": in a flat object. The quote in front of the key is
 * part of the match, so "offset" never finds "raw_offset" or "dst_offset". */
static const char *json_val(const char *body, const char *key)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(body, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ') p++;
    return p;
}

static bool json_str(const char *body, const char *key, char *out, int cap)
{
    const char *p = json_val(body, key);
    if (!p || *p != '"' || cap < 1) return false;
    p++;
    int n = 0;
    while (*p && *p != '"' && n < cap - 1) {
        if (*p == '\\' && p[1]) p++;        /* "Europe\/Berlin" */
        out[n++] = *p++;
    }
    out[n] = 0;
    return *p == '"';
}

static bool json_long(const char *body, const char *key, long *v)
{
    const char *p = json_val(body, key);
    if (!p) return false;
    char *end;
    long x = strtol(p, &end, 10);
    if (end == p) return false;
    *v = x;
    return true;
}

static bool json_bool(const char *body, const char *key, bool *v)
{
    const char *p = json_val(body, key);
    if (!p) return false;
    if (strncmp(p, "true", 4) == 0)  { *v = true;  return true; }
    if (strncmp(p, "false", 5) == 0) { *v = false; return true; }
    return false;
}

/* ── Offsets and instants ──────────────────────────────────────────────── */

static bool offset_ok(long v)
{
    return v >= -14 * 3600 && v <= 14 * 3600 && v % 900 == 0;
}

bool xtz_parse_offset(const char *s, int *off_s)
{
    if (!s) return false;
    while (*s == ' ') s++;
    int sign = 1;
    if (*s == '+') s++;
    else if (*s == '-') { sign = -1; s++; }
    if (!isdigit((unsigned char)*s)) return false;
    int h = 0, m = 0, nd = 0;
    while (isdigit((unsigned char)*s) && nd < 2) { h = h * 10 + (*s - '0'); s++; nd++; }
    if (*s == ':') {
        s++;
        if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) return false;
        m = (s[0] - '0') * 10 + (s[1] - '0');
        s += 2;
    } else if (nd == 2 && isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1])) {
        m = (s[0] - '0') * 10 + (s[1] - '0');           /* "+0545" */
        s += 2;
    }
    if (*s && *s != ' ' && *s != '"' && *s != ',' && *s != '}') return false;
    if (m >= 60) return false;
    long v = (long)sign * (h * 3600L + m * 60L);
    if (!offset_ok(v)) return false;
    *off_s = (int)v;
    return true;
}

void xtz_format_offset(int off_s, char *out, int cap)
{
    int a = off_s < 0 ? -off_s : off_s;
    snprintf(out, (size_t)cap, "%c%02d:%02d", off_s < 0 ? '-' : '+',
             a / 3600, (a / 60) % 60);
}

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's
 * days_from_civil): the conversion timegm() would do, without a TZ. */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = (int)(y - era * 400);
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* "2026-10-25T01:00:00+00:00" (fractions and "Z" allowed) to Unix time. */
static bool iso_to_unix(const char *s, int64_t *out)
{
    int Y, M, D, h, mi, se;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &mi, &se) != 6)
        return false;
    const char *p = s + 19;
    if (*p == '.') { p++; while (isdigit((unsigned char)*p)) p++; }
    int off = 0;
    if (*p != 'Z' && *p && !xtz_parse_offset(p, &off)) return false;
    *out = days_from_civil(Y, M, D) * 86400 + h * 3600 + mi * 60 + se - off;
    return true;
}

/* ── The two services' answers ─────────────────────────────────────────── */

bool xtz_parse_worldtime(const char *body, xtz_t *out)
{
    if (!body || !out) return false;
    memset(out, 0, sizeof *out);
    char s[40];
    if (!json_str(body, "utc_offset", s, sizeof s) ||
        !xtz_parse_offset(s, &out->off_s))
        return false;
    json_str(body, "timezone", out->zone, sizeof out->zone);
    out->source = "worldtimeapi.org";

    /* In daylight saving, the service says when it ends and what the offset
     * is then (the zone's raw offset). Outside it, it says nothing about
     * the next start, and the caller asks again on the hour instead. */
    bool dst = false;
    long raw;
    char until[40];
    if (json_bool(body, "dst", &dst) && dst &&
        json_long(body, "raw_offset", &raw) && offset_ok(raw) &&
        json_str(body, "dst_until", until, sizeof until) &&
        iso_to_unix(until, &out->change_utc)) {
        out->has_change = true;
        out->off_after = (int)raw;
    }
    return true;
}

bool xtz_parse_ipapi(const char *body, xtz_t *out)
{
    if (!body || !out) return false;
    memset(out, 0, sizeof *out);
    char st[16];
    long off;
    if (!json_str(body, "status", st, sizeof st) || strcmp(st, "success") != 0 ||
        !json_long(body, "offset", &off) || !offset_ok(off))
        return false;
    out->off_s = (int)off;
    json_str(body, "timezone", out->zone, sizeof out->zone);
    out->source = "ip-api.com";
    return true;
}

/* ── Asking ────────────────────────────────────────────────────────────── */

#ifndef XTZ_HOST_TEST

#include "esp_log.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "xtz";

/* The whole answer, headers and all. Static: never on a small task's stack
 * and never a malloc that fails the day the heap is short. One caller. */
static char s_rx[1024];

static const char *http_get(const char *host, const char *path)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, "80", &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "%s: no address", host);
        return NULL;
    }
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) { freeaddrinfo(res); return NULL; }
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int ok = connect(fd, res->ai_addr, res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!ok) {
        ESP_LOGW(TAG, "%s: no connection", host);
        close(fd);
        return NULL;
    }
    char req[192];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: xprs-station\r\n"
                     "Connection: close\r\n\r\n", path, host);
    int got = 0;
    if (send(fd, req, (size_t)n, 0) == n) {
        while (got < (int)sizeof s_rx - 1) {
            int r = recv(fd, s_rx + got, sizeof s_rx - 1 - (size_t)got, 0);
            if (r <= 0) break;
            got += r;
        }
    }
    close(fd);
    s_rx[got] = 0;
    if (strncmp(s_rx, "HTTP/1.", 7) != 0 || strncmp(s_rx + 8, " 200", 4) != 0) {
        char *eol = strchr(s_rx, '\r');
        if (eol) *eol = 0;
        ESP_LOGW(TAG, "%s: %s", host, got ? s_rx : "no answer");
        return NULL;
    }
    const char *body = strstr(s_rx, "\r\n\r\n");
    return body ? body + 4 : NULL;
}

bool xtz_lookup(xtz_t *out)
{
    const char *b = http_get("worldtimeapi.org", "/api/ip");
    if (b && xtz_parse_worldtime(b, out)) return true;
    b = http_get("ip-api.com", "/json/?fields=status,timezone,offset");
    if (b && xtz_parse_ipapi(b, out)) return true;
    return false;
}

#endif /* XTZ_HOST_TEST */
