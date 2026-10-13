/* xprs_setup.c -- see the header. */
#include "xprs_setup.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "xprs_tz.h"

static const char *const k_keys[] = {
    "ssid", "pass", "nsec", "wifi", "nick", "zone", "ap", "key", "lora",
    "freq", "region", "call",
};

bool xsetup_is_key(const char *key)
{
    if (!key) return false;
    for (size_t i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
        if (strcmp(key, k_keys[i]) == 0) return true;
    return false;
}

bool xsetup_is_secret(const char *key)
{
    return key && (strcmp(key, "ssid") == 0 || strcmp(key, "pass") == 0 ||
                   strcmp(key, "nsec") == 0);
}

/* One line, [s, e): `key:value`, the key the grammar's (4.1: a lowercase
 * letter, then up to seven letters and digits). */
static bool split_line(const char *s, const char *e, xsetup_kv_t *kv)
{
    if (e > s && e[-1] == '\r') e--;
    const char *c = memchr(s, ':', (size_t)(e - s));
    if (!c || c == s || c - s >= (int)sizeof kv->key) return false;
    if (*s < 'a' || *s > 'z') return false;
    for (const char *p = s + 1; p < c; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) return false;
    size_t vl = (size_t)(e - (c + 1));
    if (vl >= sizeof kv->val) return false;
    memcpy(kv->key, s, (size_t)(c - s));
    kv->key[c - s] = 0;
    memcpy(kv->val, c + 1, vl);
    kv->val[vl] = 0;
    return true;
}

int xsetup_lines(const char *plain, xsetup_kv_t *kv, int max)
{
    if (!plain || !kv || max <= 0) return -1;
    const char *nl = strchr(plain, '\n');
    size_t first = nl ? (size_t)(nl - plain) : strlen(plain);
    if (first && plain[first - 1] == '\r') first--;
    if (first != 7 || strncmp(plain, "cmd:set", 7) != 0) return -1;

    int n = 0;
    const char *s = nl ? nl + 1 : plain + strlen(plain);
    while (*s) {
        const char *e = strchr(s, '\n');
        if (!e) e = s + strlen(s);
        if (e > s) {                              /* a blank line is nothing */
            if (n >= max || !split_line(s, e, &kv[n])) return -1;
            n++;
        }
        s = *e ? e + 1 : e;
    }
    return n;
}

static bool printable(const char *v)
{
    for (; *v; v++)
        if ((unsigned char)*v < 0x20 || (unsigned char)*v > 0x7e) return false;
    return true;
}

static bool c_digit(char c) { return c >= '0' && c <= '9'; }
static bool c_upper(char c) { return c >= 'A' && c <= 'Z'; }

int xsetup_call_norm(const char *in, char *out, size_t cap)
{
    if (!in || !out || !cap) return -1;
    while (*in == ' ' || *in == '\t') in++;
    size_t n = strlen(in);
    while (n && (in[n - 1] == ' ' || in[n - 1] == '\t')) n--;
    if (n >= cap) return -1;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    out[n] = 0;
    return (int)n;
}

/*
 * The app's xprsIssuedCallsignRefusal (lib/services/xprs/xprs_callsign.dart),
 * rule for rule, so a phone and a station refuse the same callsigns. It
 * rejects only what certainly was not issued: XPRS.md 6.4.2 is explicit that
 * nothing in the format can tell whether a callsign was issued to the person
 * setting it, so a shape this file has not met still gets through.
 */
const char *xsetup_call_refusal(const char *val)
{
    if (!val) return "missing value";
    if (strcmp(val, "derived") == 0) return NULL;
    char c[XSETUP_VAL_MAX];
    int n = xsetup_call_norm(val, c, sizeof c);
    if (n < 0) return "call: too long";
    if (n == 0) return "call: the callsign on your licence, or derived";
    for (int i = 0; i < n; i++)
        if (!c_upper(c[i]) && !c_digit(c[i]) && c[i] != '-' && c[i] != '/')
            return "call: letters, digits, - and / only";
    if (n > XSETUP_CALL_MAX) return "call: at most 11 characters";
    /* 3: X1 to X5 and a few key characters is a name a key generated. */
    if (n >= 3 && c[0] == 'X' && c[1] >= '1' && c[1] <= '5')
        return "call: X1 to X5 come from a key, no authority issues them";
    /* 3.2: a node number of another network, the whole string. */
    if (n == 10 && c[0] == 'M' && (c[1] == 'T' || c[1] == 'C')) {
        bool hex = true;
        for (int i = 2; i < 10; i++)
            if (!c_digit(c[i]) && !(c[i] >= 'A' && c[i] <= 'F')) hex = false;
        if (hex) return "call: a node number of another network";
    }
    const char *dash = strchr(c, '-'), *slash = strchr(c, '/');
    if (dash && strchr(dash + 1, '-')) return "call: one - only, as in CT1ABC-9";
    if (slash && strchr(slash + 1, '/')) return "call: one / only, as in G0XYZ/P";
    if (c[0] == '-' || c[0] == '/') return "call: starts with a letter or a digit";
    if (c[n - 1] == '-' || c[n - 1] == '/') return "call: finish the suffix, or remove it";
    if (slash && dash && slash < dash) return "call: the -number goes before the /";
    /* The base: up to the first - or /. */
    int base = n;
    if (dash) base = (int)(dash - c);
    else if (slash) base = (int)(slash - c);
    if (dash) {
        /* 3.1: a device number, 1 to 99, and never -0. */
        const char *d = dash + 1;
        int dl = slash ? (int)(slash - d) : (int)strlen(d);
        if (dl < 1 || dl > 2 || !c_digit(d[0]) || (dl == 2 && !c_digit(d[1])) ||
            d[0] == '0')
            return "call: a device number runs 1 to 99, as in CT1ABC-9";
    }
    if (slash && strlen(slash + 1) > 4) return "call: up to 4 after the /";
    bool letter = false, digit = false;
    for (int i = 0; i < base; i++) {
        if (c_upper(c[i])) letter = true;
        else if (c_digit(c[i])) digit = true;
        else return "call: letters and digits before the suffix";
    }
    if (!digit) return "call: a callsign has a digit in it";
    if (!letter) return "call: a callsign has a letter in it";
    /* A prefix of one to three, then the numeral that separates it. */
    if (!((base > 1 && c_digit(c[1])) || (base > 2 && c_digit(c[2])) ||
          (base > 3 && c_digit(c[3]))))
        return "call: not shaped like a callsign";
    return NULL;
}

const char *xsetup_check(const char *key, const char *val)
{
    if (!key || !val) return "missing value";
    size_t n = strlen(val);

    if (strcmp(key, "ssid") == 0) {
        /* 802.11: up to 32 bytes, and not nothing. Bytes rather than
         * characters, so a name with accents that fits the air fits here. */
        if (n < 1 || n > 32) return "ssid: 1 to 32 bytes";
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)val[i] < 0x20) return "ssid: no control characters";
        return NULL;
    }
    if (strcmp(key, "pass") == 0) {
        /* WPA2: a passphrase of 8 to 63 printable ASCII characters. The
         * 64-hex form of the key is not taken: the station keeps a passphrase
         * and nothing else. An empty one is an open network, which is ssid:
         * with no pass: and wifi:join, not an empty pass:. */
        if (n < 8 || n > 63) return "pass: 8 to 63 characters";
        if (!printable(val)) return "pass: printable ASCII only";
        return NULL;
    }
    if (strcmp(key, "nsec") == 0) {
        if (n != 63 || strncmp(val, "nsec1", 5) != 0) return "nsec: nsec1 and 58 more";
        return NULL;
    }
    if (strcmp(key, "wifi") == 0) {
        if (strcmp(val, "join") == 0 || strcmp(val, "off") == 0) return NULL;
        return "wifi: join or off";
    }
    if (strcmp(key, "ap") == 0) {
        if (strcmp(val, "on") == 0 || strcmp(val, "off") == 0) return NULL;
        return "ap: on or off";
    }
    if (strcmp(key, "key") == 0) {
        if (strcmp(val, "new") == 0) return NULL;
        return "key: new";
    }
    if (strcmp(key, "lora") == 0) {
        /* 14.8: the LoRa mode. The grammar names all four; whether this
         * firmware HAS one is the station's answer, code:501, not a
         * malformed command (a small board may be built without one, and
         * `both` needs PSRAM and a configured channel). */
        if (strcmp(val, "xprs") == 0 || strcmp(val, "meshtastic") == 0 ||
            strcmp(val, "meshcore") == 0 || strcmp(val, "both") == 0)
            return NULL;
        return "lora: xprs, meshtastic, meshcore or both";
    }
    if (strcmp(key, "freq") == 0) {
        /* 14.8: the channel, in MHz (433.9) or in Hz (433900000), and
         * `preset` to go back to the region's own. Not every board is an
         * 868 MHz board, and a 433 community picks its own channel. The
         * station answers what its radio can tune (code:400 outside
         * 150-960 MHz); whether it is legal where it stands is the
         * operator's to answer, as the power ceiling already is. */
        if (strcmp(val, "preset") == 0) return NULL;
        if (n < 1) return "freq: MHz, Hz, or preset";
        /* The same shape section 14 writes a frequency in, `145.500MHz`,
         * with the suffix optional and hertz allowed. */
        size_t digits = n;
        if (n > 3 && (val[n - 3] == 'M' || val[n - 3] == 'm') &&
            (val[n - 2] == 'H' || val[n - 2] == 'h') &&
            (val[n - 1] == 'z' || val[n - 1] == 'Z'))
            digits = n - 3;
        if (!digits) return "freq: MHz, Hz, or preset";
        int dots = 0;
        for (size_t i = 0; i < digits; i++) {
            char c = val[i];
            if (c == '.') { if (++dots > 1) return "freq: one decimal point"; }
            else if (c < '0' || c > '9') return "freq: digits, or preset";
        }
        double v = atof(val);
        double hz = v > 10000.0 ? v : v * 1000000.0;
        if (hz < 150000000.0 || hz > 960000000.0)
            return "freq: 150 to 960 MHz is what the radio can tune";
        return NULL;
    }
    if (strcmp(key, "region") == 0) {
        /* A preset of the running mode (14.8). Which rows exist is the
         * station's answer (code:501), not the grammar's. */
        if (n < 2 || n > 8) return "region: a preset name, like eu or eu-433";
        for (size_t i = 0; i < n; i++) {
            char c = val[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
                return "region: lowercase letters, digits and -";
        }
        return NULL;
    }
    if (strcmp(key, "nick") == 0) {
        /* 6.3.1: one to sixteen ASCII letters, digits, - and _. */
        if (n < 1 || n > 16) return "nick: 1 to 16 characters";
        for (size_t i = 0; i < n; i++) {
            char c = val[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_'))
                return "nick: letters, digits, - and _";
        }
        return NULL;
    }
    if (strcmp(key, "call") == 0) return xsetup_call_refusal(val);
    if (strcmp(key, "zone") == 0) {
        int off;
        if (strcmp(val, "auto") == 0) return NULL;
        if (xtz_parse_offset(val, &off)) return NULL;
        return "zone: auto or an offset such as +01:00";
    }
    return "not a setup key";
}
