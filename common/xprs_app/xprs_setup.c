/* xprs_setup.c -- see the header. */
#include "xprs_setup.h"

#include <string.h>
#include <stdio.h>

#include "xprs_tz.h"

static const char *const k_keys[] = {
    "ssid", "pass", "nsec", "wifi", "nick", "zone", "ap", "key",
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
    if (strcmp(key, "zone") == 0) {
        int off;
        if (strcmp(val, "auto") == 0) return NULL;
        if (xtz_parse_offset(val, &off)) return NULL;
        return "zone: auto or an offset such as +01:00";
    }
    return "not a setup key";
}
