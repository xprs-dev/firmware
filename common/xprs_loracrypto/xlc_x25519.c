/*
 * xlc_x25519.c -- the field arithmetic and the Montgomery ladder (RFC 7748).
 *
 * Moved here unchanged from xprs_meshtastic/mt_pki.c, where it was written
 * from the RFC, so that Ed25519 in xlc_ed25519.c uses the same field and
 * neither network carries its own copy. About 1.3 KB of stack at the
 * deepest point, which is what the bearer task's headroom is measured
 * against (docs/esp32.md).
 */

#include "xlc.h"
#include "xlc_fe.h"

#include <string.h>

void xlc_fe_car(fe o)
{
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= (int64_t)((uint64_t)c << 16); /* c can be negative, and a
                                                  * shift of that is undefined */
    }
}

void xlc_fe_sel(fe p, fe q, int b)
{
    int64_t c = ~(int64_t)(b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void xlc_fe_pack(uint8_t o[32], const fe n)
{
    fe m, t;
    memcpy(t, n, sizeof t);
    xlc_fe_car(t);
    xlc_fe_car(t);
    xlc_fe_car(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        xlc_fe_sel(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

void xlc_fe_unpack(fe o, const uint8_t n[32])
{
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

void xlc_fe_add(fe o, const fe a, const fe b)
{
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

void xlc_fe_sub(fe o, const fe a, const fe b)
{
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

void xlc_fe_mul(fe o, const fe a, const fe b)
{
    int64_t t[31];
    memset(t, 0, sizeof t);
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    xlc_fe_car(o);
    xlc_fe_car(o);
}

void xlc_fe_inv(fe o, const fe i)
{
    fe c;
    memcpy(c, i, sizeof c);
    for (int a = 253; a >= 0; a--) {
        xlc_fe_mul(c, c, c);
        if (a != 2 && a != 4) xlc_fe_mul(c, c, i);
    }
    memcpy(o, c, sizeof c);
}

void xlc_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    static const fe k121665 = { 0xDB41, 1 };
    uint8_t z[32];
    fe x, a, b, c, d, e, f;
    memcpy(z, scalar, 32);
    z[31] = (uint8_t)((z[31] & 127) | 64);
    z[0] &= 248;
    xlc_fe_unpack(x, point);
    memset(a, 0, sizeof a);
    memset(c, 0, sizeof c);
    memset(d, 0, sizeof d);
    memcpy(b, x, sizeof b);
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        xlc_fe_sel(a, b, r);
        xlc_fe_sel(c, d, r);
        xlc_fe_add(e, a, c);
        xlc_fe_sub(a, a, c);
        xlc_fe_add(c, b, d);
        xlc_fe_sub(b, b, d);
        xlc_fe_mul(d, e, e);
        xlc_fe_mul(f, a, a);
        xlc_fe_mul(a, c, a);
        xlc_fe_mul(c, b, e);
        xlc_fe_add(e, a, c);
        xlc_fe_sub(a, a, c);
        xlc_fe_mul(b, a, a);
        xlc_fe_sub(c, d, f);
        xlc_fe_mul(a, c, k121665);
        xlc_fe_add(a, a, d);
        xlc_fe_mul(c, c, a);
        xlc_fe_mul(a, d, f);
        xlc_fe_mul(d, b, x);
        xlc_fe_mul(b, e, e);
        xlc_fe_sel(a, b, r);
        xlc_fe_sel(c, d, r);
    }
    xlc_fe_inv(c, c);
    xlc_fe_mul(a, a, c);
    xlc_fe_pack(out, a);
}

void xlc_x25519_base(uint8_t pub[32], const uint8_t priv[32])
{
    static const uint8_t nine[32] = { 9 };
    xlc_x25519(pub, priv, nine);
}

/* o = i^((p-5)/8): the exponent that turns a square into its root candidate
 * when Ed25519 recovers x from a packed y (RFC 8032 5.1.3). The ladder is
 * the same shape as xlc_fe_inv's, with the two skipped squarings moved. */
void xlc_fe_pow2523(fe o, const fe i)
{
    fe c;
    memcpy(c, i, sizeof c);
    for (int a = 250; a >= 0; a--) {
        xlc_fe_mul(c, c, c);
        if (a != 1) xlc_fe_mul(c, c, i);
    }
    memcpy(o, c, sizeof c);
}
