/* Parsing and range-checking a signed script bundle. See xs_bundle.h.
 *
 * Deliberately free of ESP-IDF: no logging, no heap, no partition API, so the
 * same code runs under the host test in test/. The format is the one thing the
 * signing tool and the station must agree about byte for byte, and the place
 * to find out that they do not is a desk, not a roof. */

#include "xs_bundle.h"
#include "xprssig.h"

#include <string.h>
#include <stdio.h>

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* Copy a fixed-width, NUL-padded field out as a C string. The field is not
 * required to be terminated in the file, which is why this exists. */
static void field(char *dst, size_t dstcap, const uint8_t *src, size_t n)
{
    size_t i = 0;
    for (; i < n && i < dstcap - 1 && src[i]; i++) dst[i] = (char)src[i];
    dst[i] = '\0';
}

bool xsb_parse(const uint8_t *buf, size_t len, xsb_t *out)
{
    if (!buf || !out || len < XSB_BODY_OFF) return false;
    memset(out, 0, sizeof *out);

    if (memcmp(buf, XSB_MAGIC, 4) != 0) return false;
    if (rd16(buf + 4) != XSB_FORMAT) return false;

    uint16_t nmod = rd16(buf + 6);
    if (nmod == 0 || nmod > XSB_MODS_MAX) return false;

    field(out->id,      sizeof out->id,      buf + 8,  XSB_ID_MAX);
    field(out->version, sizeof out->version, buf + 24, XSB_VER_MAX);
    field(out->sig,     sizeof out->sig,     buf + 48, XSB_SIG_MAX);
    if (!out->id[0] || !out->version[0]) return false;

    out->signed_len = rd32(buf + 112);
    out->nmod = nmod;

    /* The signature must cover the whole of the rest of the bundle. If it
     * covers less, the bytes past it are unsigned and would be handed to the
     * VM on somebody else's authority. */
    if (out->signed_len != len - XSB_BODY_OFF) return false;

    size_t table = XSB_BODY_OFF;
    size_t table_bytes = (size_t)nmod * XSB_MOD_ENTRY;
    size_t types_off = table + table_bytes;
    if (types_off + (size_t)XSB_TYPES_MAX * XSB_TYPE_LEN > len) return false;

    for (uint16_t i = 0; i < nmod; i++) {
        const uint8_t *e = buf + table + (size_t)i * XSB_MOD_ENTRY;
        xsb_module_t *m = &out->mod[i];
        field(m->name, sizeof m->name, e, XSB_NAME_MAX);
        m->off     = rd32(e + 24);
        m->len     = rd32(e + 28);
        m->tick_ms = rd16(e + 32);
        m->flags   = rd16(e + 34);

        if (!m->name[0]) return false;
        if (m->len == 0) return false;
        /* Payloads live after the header, the table and the type block, and
         * must fit inside the bundle. Checked here so no caller has to. */
        if (m->off < types_off + (size_t)XSB_TYPES_MAX * XSB_TYPE_LEN) return false;
        if (m->off > len || m->len > len - m->off) return false;
        /* A tick faster than 100 ms is a script asking to be the busiest
         * thing on core 1. The floor is enforced here rather than trusted. */
        if (m->tick_ms && m->tick_ms < 100) m->tick_ms = 100;
    }

    for (int i = 0; i < XSB_TYPES_MAX; i++)
        field(out->types[i], sizeof out->types[i],
              buf + types_off + (size_t)i * XSB_TYPE_LEN, XSB_TYPE_LEN);

    return true;
}

int xsb_signed_line(char *out, size_t cap, const char *board,
                    const xsb_t *b, const char *sha_hex)
{
    if (!out || !board || !b || !sha_hex) return -1;
    int n = snprintf(out, cap, "xprsscr1 %s %s %s %u %s",
                     board, b->id, b->version,
                     (unsigned)b->signed_len, sha_hex);
    if (n <= 0 || (size_t)n >= cap) return -1;
    return n;
}

bool xsb_wants_type(const xsb_t *b, const char *type)
{
    if (!b || !type || !type[0]) return false;
    /* Called on whatever task heard the packet, so: no allocation, no lock,
     * at most eight short string compares against a table filled at load. */
    for (int i = 0; i < XSB_TYPES_MAX; i++) {
        if (!b->types[i][0]) continue;
        if (strcmp(b->types[i], type) == 0) return true;
    }
    return false;
}

static void hexify(const uint8_t *in, size_t n, char *out)
{
    static const char *h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

bool xsb_verify(const uint8_t *buf, size_t len, const xsb_t *b,
                const char *board, const uint8_t pub[32])
{
    if (!buf || !b || !board || !pub) return false;
    if (len < XSB_BODY_OFF) return false;
    if (b->signed_len != len - XSB_BODY_OFF) return false;

    /* An unsigned bundle is not a special case to be handled leniently. It
     * is the ordinary case on a partition that was erased, half-written, or
     * filled by somebody who does not have the key. */
    if (strlen(b->sig) != XPRSSIG_B85_LEN) return false;

    uint8_t sig[XPRSSIG_LEN];
    if (xprssig_b85_decode(b->sig, XPRSSIG_B85_LEN, sig, sizeof sig)
        != XPRSSIG_LEN) return false;

    /* The hash is over the body only. The header is excluded because it
     * carries the signature itself, and a field cannot cover itself. */
    uint8_t sha[32];
    xsb_sha256(buf + XSB_BODY_OFF, b->signed_len, sha);
    char shahex[65];
    hexify(sha, 32, shahex);

    char line[200];
    if (xsb_signed_line(line, sizeof line, board, b, shahex) < 0) return false;

    uint8_t digest[32];
    xsb_sha256((const uint8_t *)line, strlen(line), digest);
    return xprssig_verify(digest, sig, pub);
}
