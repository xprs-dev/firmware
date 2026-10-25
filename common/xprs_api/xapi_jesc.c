/*
 * xapi_jesc -- a byte string as the inside of a JSON string.
 *
 * No IDF here, so it is tested on the host (test_jesc_host.sh).
 *
 * JSON is UTF-8. Two things must hold at once, and the first version kept
 * only the second:
 *
 *  1. Text people write passes through as written. "Eh la" with its accent
 *     is the two bytes C3 A1, and escaping each byte on its own sent
 *     Ã¡ -- which every reader decodes as two Latin-1 characters,
 *     "Ã¡". Every accented message the station relayed reached the phone
 *     app, the web chat and the API garbled (2026-10-24). A valid UTF-8
 *     sequence is copied whole.
 *  2. A byte that is NOT part of valid UTF-8 never reaches the reply raw.
 *     A record recovered from a card need not be text, and one stray 0xb7
 *     once made a whole reply undecodable. Such a byte is still escaped as
 *     its code point, which is valid JSON and keeps it visible.
 *
 * Valid means RFC 3629: no overlong forms, no surrogates, nothing past
 * U+10FFFF. Control characters and DEL are escaped as before.
 */
#include <stdio.h>
#include <stddef.h>

int xapi_jesc(char *out, size_t cap, const char *in, int inlen);

/* Length of the valid UTF-8 sequence starting at [p], or 0. */
static int utf8_len(const unsigned char *p, int avail)
{
    unsigned char c = p[0];
    int n;
    unsigned char lo = 0x80, hi = 0xbf;      /* range of the second byte */
    if (c >= 0xc2 && c <= 0xdf) n = 2;
    else if (c == 0xe0) { n = 3; lo = 0xa0; }
    else if (c >= 0xe1 && c <= 0xec) n = 3;
    else if (c == 0xed) { n = 3; hi = 0x9f; }   /* not a surrogate */
    else if (c >= 0xee && c <= 0xef) n = 3;
    else if (c == 0xf0) { n = 4; lo = 0x90; }
    else if (c >= 0xf1 && c <= 0xf3) n = 4;
    else if (c == 0xf4) { n = 4; hi = 0x8f; }   /* not past U+10FFFF */
    else return 0;
    if (avail < n) return 0;
    if (p[1] < lo || p[1] > hi) return 0;
    for (int k = 2; k < n; k++)
        if (p[k] < 0x80 || p[k] > 0xbf) return 0;
    return n;
}

int xapi_jesc(char *out, size_t cap, const char *in, int inlen)
{
    size_t o = 0;
    if (cap == 0) return 0;
    const unsigned char *u = (const unsigned char *)in;
    for (int i = 0; i < inlen && u[i]; i++) {
        unsigned char c = u[i];
        if (o + 7 >= cap) break;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c < 0x20 || c == 0x7f)
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        else if (c < 0x80) out[o++] = (char)c;
        else {
            int n = 0;
            /* Count only bytes before the terminator and inside inlen. */
            int avail = 0;
            while (avail < 4 && i + avail < inlen && u[i + avail]) avail++;
            n = utf8_len(u + i, avail);
            if (n) {
                if (o + (size_t)n + 1 >= cap) break;
                for (int k = 0; k < n; k++) out[o++] = (char)u[i + k];
                i += n - 1;
            } else {
                o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
            }
        }
    }
    out[o] = 0;
    return (int)o;
}
