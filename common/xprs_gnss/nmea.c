/* NMEA 0183, RMC and GGA (nmea.h). */
#include "nmea.h"

#include <string.h>

void nmea_init(nmea_t *p) { memset(p, 0, sizeof *p); }

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* The [i]th comma-separated field after the sentence name (0 = the first
 * value). Returns its start and writes its length; NULL when absent. */
static const char *field(const char *s, int i, int *len)
{
    const char *p = strchr(s, ',');
    while (p && i-- > 0) p = strchr(p + 1, ',');
    if (!p) return NULL;
    p++;
    const char *e = p;
    while (*e && *e != ',' && *e != '*') e++;
    *len = (int)(e - p);
    return p;
}

/* A decimal number with up to [frac] fraction digits, scaled by 10^frac. */
static bool num(const char *v, int n, int frac, int64_t *out)
{
    if (n <= 0) return false;
    bool neg = false;
    int i = 0;
    if (v[0] == '-') { neg = true; i = 1; }
    int64_t x = 0;
    int f = -1;
    for (; i < n; i++) {
        if (v[i] == '.') { if (f >= 0) return false; f = 0; continue; }
        if (v[i] < '0' || v[i] > '9') return false;
        if (f >= 0) { if (f >= frac) continue; f++; }
        x = x * 10 + (v[i] - '0');
    }
    if (f < 0) f = 0;
    for (; f < frac; f++) x *= 10;
    *out = neg ? -x : x;
    return true;
}

/* ddmm.mmmm or dddmm.mmmm to millionths of a degree. */
static bool coord(const char *v, int n, const char *h, int hn, int32_t *out)
{
    int64_t x;                   /* dddmm.mmmmmm scaled by 1e6 */
    if (!num(v, n, 6, &x) || hn != 1 || x < 0) return false;
    int32_t deg = (int32_t)(x / 100000000);      /* dd(d) */
    int64_t min_e6 = x - (int64_t)deg * 100000000;
    int32_t e6 = deg * 1000000 + (int32_t)(((int64_t)min_e6 + 30) / 60);
    if (*h == 'S' || *h == 'W') e6 = -e6;
    else if (*h != 'N' && *h != 'E') return false;
    *out = e6;
    return true;
}

static uint32_t epoch_of(int y, int mo, int d, int hh, int mm, int ss)
{
    y -= mo <= 2;
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + doe - 719468;
    return (uint32_t)(days * 86400L + hh * 3600L + mm * 60L + ss);
}

static int two(const char *v) { return (v[0] - '0') * 10 + (v[1] - '0'); }

bool nmea_sentence(nmea_t *p, const char *s)
{
    int n = (int)strlen(s);
    if (n < 10 || s[0] != '$') return false;
    const char *star = strrchr(s, '*');
    if (!star || star + 3 > s + n) { p->bad++; return false; }
    uint8_t ck = 0;
    for (const char *c = s + 1; c < star; c++) ck ^= (uint8_t)*c;
    int h1 = hexv(star[1]), h2 = hexv(star[2]);
    if (h1 < 0 || h2 < 0 || ck != (uint8_t)(h1 * 16 + h2)) { p->bad++; return false; }
    p->sentences++;

    const char *v, *h;
    int vn, hn;
    if (!strncmp(s + 3, "RMC,", 4)) {
        const char *t = field(s, 0, &vn);
        const char *st = field(s, 1, &hn);
        const char *dt;
        int dn = 0;
        p->rmc_ok = st && hn == 1 && *st == 'A';
        dt = field(s, 8, &dn);
        if (t && vn >= 6 && dt && dn == 6) {
            int y = 2000 + two(dt + 4);
            p->fix.utc = epoch_of(y, two(dt + 2), two(dt), two(t), two(t + 2), two(t + 4));
        }
        if (p->rmc_ok && (v = field(s, 2, &vn)) && (h = field(s, 3, &hn))) {
            int32_t lat, lon;
            const char *v2, *h2b;
            int vn2, hn2;
            if (coord(v, vn, h, hn, &lat) && (v2 = field(s, 4, &vn2)) && (h2b = field(s, 5, &hn2)) &&
                coord(v2, vn2, h2b, hn2, &lon)) {
                p->fix.lat_e6 = lat;
                p->fix.lon_e6 = lon;
            }
        }
        p->fix.valid = p->rmc_ok && p->fix.quality > 0;
        return true;
    }
    if (!strncmp(s + 3, "GGA,", 4)) {
        int64_t x;
        if ((v = field(s, 5, &vn)) && num(v, vn, 0, &x)) p->fix.quality = (uint8_t)x;
        if ((v = field(s, 6, &vn)) && num(v, vn, 0, &x)) p->fix.sats = (uint8_t)x;
        if ((v = field(s, 7, &vn)) && num(v, vn, 1, &x)) p->fix.hdop_x10 = (uint16_t)x;
        if ((v = field(s, 8, &vn)) && num(v, vn, 1, &x)) p->fix.alt_dm = x;
        if (p->fix.quality > 0 && (v = field(s, 1, &vn)) && (h = field(s, 2, &hn))) {
            int32_t lat, lon;
            const char *v2, *h2b;
            int vn2, hn2;
            if (coord(v, vn, h, hn, &lat) && (v2 = field(s, 3, &vn2)) && (h2b = field(s, 4, &hn2)) &&
                coord(v2, vn2, h2b, hn2, &lon)) {
                p->fix.lat_e6 = lat;
                p->fix.lon_e6 = lon;
            }
        }
        p->fix.valid = p->rmc_ok && p->fix.quality > 0;
        return true;
    }
    return false;
}

bool nmea_feed(nmea_t *p, char c)
{
    if (c == '$') { p->n = 0; p->line[p->n++] = c; return false; }
    if (!p->n) return false;
    if (c == '\r' || c == '\n') {
        p->line[p->n] = 0;
        p->n = 0;
        return nmea_sentence(p, p->line);
    }
    if (p->n >= (int)sizeof p->line - 1) { p->n = 0; p->bad++; return false; }
    p->line[p->n++] = c;
    return false;
}

int nmea_acc_m(const nmea_fix_t *f)
{
    if (!f->hdop_x10) return 0;
    int a = (f->hdop_x10 * 5 + 5) / 10;
    return a < 1 ? 1 : a;
}
