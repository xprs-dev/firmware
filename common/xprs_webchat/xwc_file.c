/* xwc_file.c -- see the header. */
#include "xwc_file.h"

#include <string.h>

static int b64u_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool xwcf_parse(const char *ref, uint8_t sha[32], char ext[XWCF_EXT_MAX + 1])
{
    if (!ref) return false;
    const char *dot = strchr(ref, '.');
    if (!dot || strchr(dot + 1, '.')) return false;
    size_t hl = (size_t)(dot - ref), el = strlen(dot + 1);
    if (el < 1 || el > XWCF_EXT_MAX) return false;
    for (size_t i = 0; i < el; i++) {
        char c = dot[1 + i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    if (hl == 64) {
        for (int i = 0; i < 32; i++) {
            int a = hex_val(ref[2 * i]), b = hex_val(ref[2 * i + 1]);
            if (a < 0 || b < 0) return false;
            sha[i] = (uint8_t)(a << 4 | b);
        }
    } else if (hl == 43) {
        /* 43 characters are 258 bits: 32 bytes and two bits that must be 0. */
        uint32_t acc = 0;
        int bits = 0, n = 0;
        for (size_t i = 0; i < 43; i++) {
            int v = b64u_val(ref[i]);
            if (v < 0) return false;
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (n < 32) sha[n++] = (uint8_t)(acc >> bits);
                acc &= (1u << bits) - 1;
            }
        }
        if (n != 32 || acc != 0) return false;
    } else {
        return false;
    }
    memcpy(ext, dot + 1, el + 1);
    return true;
}

void xwcf_hex(const uint8_t sha[32], char out[65])
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = hx[sha[i] >> 4];
        out[2 * i + 1] = hx[sha[i] & 15];
    }
    out[64] = 0;
}

const char *xwcf_type(const char *ext)
{
    static const struct { const char *e, *t; } map[] = {
        { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "png", "image/png" },
        { "gif", "image/gif" }, { "webp", "image/webp" }, { "avif", "image/avif" },
        { "bmp", "image/bmp" }, { "txt", "text/plain; charset=utf-8" },
        { "md", "text/markdown; charset=utf-8" }, { "pdf", "application/pdf" },
        { "mp3", "audio/mpeg" }, { "ogg", "audio/ogg" }, { "mp4", "video/mp4" },
    };
    for (size_t i = 0; ext && i < sizeof map / sizeof map[0]; i++)
        if (strcmp(ext, map[i].e) == 0) return map[i].t;
    return "application/octet-stream";
}
