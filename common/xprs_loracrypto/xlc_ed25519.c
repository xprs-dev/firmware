/*
 * xlc_ed25519.c -- Ed25519 signing and verification (RFC 8032), and the
 * key exchange MeshCore performs over the same identity.
 *
 * MeshCore signs every advert: a node's name and public key are believable
 * only because the signature over them is (docs, payloads: "signature: 64
 * bytes (Ed25519 signature of public key, timestamp, and app data)"). A
 * bridge that speaks for XPRS callsigns therefore has to SIGN, which is the
 * one primitive this tree did not have: Meshtastic needed X25519 alone.
 *
 * Written from RFC 8032 over the field in xlc_fe.h, the same way the
 * Montgomery ladder next door was written from RFC 7748. The curve is
 *   -x^2 + y^2 = 1 + d x^2 y^2,  d = -121665/121666,
 * and points are kept in extended coordinates (X, Y, Z, T) with
 * x = X/Z, y = Y/Z, xy = T/Z, where addition is complete: one formula for
 * every pair of points, doubling included, so there is no branch that
 * depends on a secret.
 *
 * Checked against RFC 8032's own vectors and against OpenSSL in
 * test_xlc_host.c.
 */

#include "xlc.h"
#include "xlc_fe.h"

#include <string.h>

/* The curve constant d, and sqrt(-1), both as field elements. Taken from
 * RFC 8032 section 5.1 and reduced into the sixteen-limb form. */
static const fe k_d = { 0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d,
                        0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f,
                        0x6cee, 0x5203 };
static const fe k_sqrtm1 = { 0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f,
                             0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d,
                             0xdf0b, 0x4fc1, 0x2480, 0x2b83 };
/* The base point B: y = 4/5, x the even root (RFC 8032 5.1). */
static const fe k_bx = { 0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525,
                         0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4,
                         0x53fe, 0xcd6e, 0x36d3, 0x2169 };
static const fe k_by = { 0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                         0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                         0x6666, 0x6666, 0x6666, 0x6666 };

/* The group order L = 2^252 + 27742317777372353535851937790883648493, as
 * the sixteen-bit limbs the reduction below walks (RFC 8032 5.1). */
static const int64_t k_L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
    0xa2, 0xde, 0xf9, 0xde, 0x14, 0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10
};

typedef fe pt[4];   /* X, Y, Z, T */

static void fe_one(fe o)
{
    memset(o, 0, sizeof(fe));
    o[0] = 1;
}

static void fe_zero(fe o)
{
    memset(o, 0, sizeof(fe));
}

static void fe_copy(fe o, const fe a)
{
    memcpy(o, a, sizeof(fe));
}

/* The identity, (0, 1, 1, 0). */
static void pt_zero(pt p)
{
    fe_zero(p[0]);
    fe_one(p[1]);
    fe_one(p[2]);
    fe_zero(p[3]);
}

static void pt_copy(pt o, const pt a)
{
    for (int i = 0; i < 4; i++) fe_copy(o[i], a[i]);
}

/*
 * p = p + q, the complete twisted-Edwards addition of RFC 8032 5.1.4.
 *
 * FIVE field elements, not the nine the formula reads as: each of a, b, c
 * and d is dead the moment e, f, g and h are built from it, so they share
 * the slots. That is 512 bytes of stack saved on a path that runs three
 * deep (verify -> scalar multiply -> here) on a task with about two
 * kilobytes to spare (docs/esp32.md, "Task stacks are heap").
 */
static void pt_add(pt p, const pt q)
{
    fe a, b, c, d, t;
    xlc_fe_sub(a, p[1], p[0]);
    xlc_fe_sub(t, q[1], q[0]);
    xlc_fe_mul(a, a, t);            /* a = (Y1-X1)(Y2-X2) */
    xlc_fe_add(b, p[0], p[1]);
    xlc_fe_add(t, q[0], q[1]);
    xlc_fe_mul(b, b, t);            /* b = (Y1+X1)(Y2+X2) */
    xlc_fe_mul(c, p[3], q[3]);
    xlc_fe_mul(c, c, k_d);
    xlc_fe_add(c, c, c);            /* c = 2 d T1 T2 */
    xlc_fe_mul(d, p[2], q[2]);
    xlc_fe_add(d, d, d);            /* d = 2 Z1 Z2 */

    xlc_fe_add(t, b, a);            /* t = h */
    xlc_fe_sub(a, b, a);            /* a = e, and b is free */
    xlc_fe_add(b, d, c);            /* b = g */
    xlc_fe_sub(c, d, c);            /* c = f, and d is free */

    xlc_fe_mul(p[0], a, c);         /* X = e f */
    xlc_fe_mul(p[1], t, b);         /* Y = h g */
    xlc_fe_mul(p[2], b, c);         /* Z = g f */
    xlc_fe_mul(p[3], a, t);         /* T = e h */
}

static void pt_sel(pt p, pt q, int b)
{
    for (int i = 0; i < 4; i++) xlc_fe_sel(p[i], q[i], b);
}

/* p = [s] q: the lowest bit first, q doubling as the bits are walked, and
 * every addition taken under a mask, so the time it takes says nothing
 * about s. */
static void pt_mul(pt p, const pt q, const uint8_t s[32])
{
    pt sum, tmp;
    pt_zero(p);
    pt_copy(sum, q);
    for (int i = 0; i < 256; i++) {
        int bit = (s[i >> 3] >> (i & 7)) & 1;
        pt_copy(tmp, p);
        pt_add(tmp, sum);
        pt_sel(p, tmp, bit);
        pt_add(sum, sum);
    }
}

/*
 * The same, for a scalar that is PUBLIC: a signature's S and the hash k in
 * a verification are on the wire, so there is nothing to leak by skipping
 * the zero bits, and one point of working space goes with them. Never call
 * this with a secret scalar.
 */
static void pt_mul_public(pt p, const pt q, const uint8_t s[32])
{
    pt sum;
    pt_copy(sum, q);                /* first: [p] may BE [q] */
    pt_zero(p);
    for (int i = 0; i < 256; i++) {
        if ((s[i >> 3] >> (i & 7)) & 1) pt_add(p, sum);
        pt_add(sum, sum);
    }
}

/* The textbook double-and-add, top bit down: p = [s] base. */
static void pt_mul_base(pt p, const uint8_t s[32])
{
    pt b;
    fe_copy(b[0], k_bx);
    fe_copy(b[1], k_by);
    fe_one(b[2]);
    xlc_fe_mul(b[3], k_bx, k_by);
    pt_mul(p, b, s);
}

/* The base point by a PUBLIC scalar (a signature's S). */
static void pt_mul_base_public(pt p, const uint8_t s[32])
{
    pt b;
    fe_copy(b[0], k_bx);
    fe_copy(b[1], k_by);
    fe_one(b[2]);
    xlc_fe_mul(b[3], k_bx, k_by);
    pt_mul_public(p, b, s);
}

/* y with x's low bit on top, the 32-byte form of a point. */
static void pt_pack(uint8_t o[32], const pt p)
{
    fe zi, x, y;
    xlc_fe_inv(zi, p[2]);
    xlc_fe_mul(x, p[0], zi);
    xlc_fe_mul(y, p[1], zi);
    xlc_fe_pack(o, y);
    uint8_t xs[32];
    xlc_fe_pack(xs, x);
    o[31] ^= (uint8_t)((xs[0] & 1) << 7);
}

static int fe_is_zero(const fe a)
{
    uint8_t s[32];
    xlc_fe_pack(s, a);
    uint8_t d = 0;
    for (int i = 0; i < 32; i++) d |= s[i];
    return d == 0;
}

/* Recover the point whose packed form is [s]: x from y by RFC 8032 5.1.3,
 * then the sign bit decides which root. False when there is none. */
static bool pt_unpack(pt p, const uint8_t s[32])
{
    fe num, den, t, x, chk;
    xlc_fe_unpack(p[1], s);
    fe_one(p[2]);
    xlc_fe_mul(num, p[1], p[1]);
    xlc_fe_mul(den, num, k_d);
    xlc_fe_sub(num, num, p[2]);        /* y^2 - 1 */
    xlc_fe_add(den, den, p[2]);        /* d y^2 + 1 */

    fe den2, den4, den6;
    xlc_fe_mul(den2, den, den);
    xlc_fe_mul(den4, den2, den2);
    xlc_fe_mul(den6, den4, den2);
    xlc_fe_mul(t, den6, num);
    xlc_fe_mul(t, t, den);
    xlc_fe_pow2523(t, t);
    xlc_fe_mul(t, t, num);
    xlc_fe_mul(t, t, den);
    xlc_fe_mul(t, t, den);
    xlc_fe_mul(x, t, den);             /* x = (num/den)^((p+3)/8) */

    xlc_fe_mul(chk, x, x);
    xlc_fe_mul(chk, chk, den);
    fe diff;
    xlc_fe_sub(diff, chk, num);
    if (!fe_is_zero(diff)) {           /* try the other root */
        xlc_fe_mul(x, x, k_sqrtm1);
        xlc_fe_mul(chk, x, x);
        xlc_fe_mul(chk, chk, den);
        xlc_fe_sub(diff, chk, num);
        if (!fe_is_zero(diff)) return false;
    }
    uint8_t xs[32];
    xlc_fe_pack(xs, x);
    if ((xs[0] & 1) != (s[31] >> 7)) {
        fe zero;
        fe_zero(zero);
        xlc_fe_sub(x, zero, x);
    }
    fe_copy(p[0], x);
    xlc_fe_mul(p[3], p[0], p[1]);
    return true;
}

/* r mod L, r being 64 bytes little-endian: the schoolbook reduction of
 * RFC 8032's reference code, kept because it is short and never secret. */
static void mod_l(uint8_t out[32], int64_t r[64])
{
    for (int i = 63; i >= 32; i--) {
        int64_t carry = 0;
        int j;
        for (j = i - 32; j < i - 12; j++) {
            r[j] += carry - 16 * r[i] * k_L[j - (i - 32)];
            carry = (r[j] + 128) >> 8;
            r[j] -= (int64_t)((uint64_t)carry << 8); /* carry can be
                                                      * negative here */
        }
        r[j] += carry;
        r[i] = 0;
    }
    int64_t carry = 0;
    for (int j = 0; j < 32; j++) {
        r[j] += carry - (r[31] >> 4) * k_L[j];
        carry = r[j] >> 8;
        r[j] &= 255;
    }
    for (int j = 0; j < 32; j++) r[j] -= carry * k_L[j];
    for (int i = 0; i < 32; i++) {
        r[i + 1] += r[i] >> 8;
        out[i] = (uint8_t)(r[i] & 255);
    }
}

static void mod_l_bytes(uint8_t out[32], const uint8_t h[64])
{
    int64_t r[64];
    for (int i = 0; i < 64; i++) r[i] = h[i];
    mod_l(out, r);
}

void xlc_ed25519_from_seed(const uint8_t seed[32], uint8_t sk[64],
                           uint8_t pub[32])
{
    uint8_t h[64];
    xlc_sha512(seed, 32, h);
    h[0] &= 248;
    h[31] = (uint8_t)((h[31] & 127) | 64);
    memcpy(sk, h, 64);              /* clamped scalar, then the prefix */
    if (pub) {
        pt a;
        pt_mul_base(a, sk);
        pt_pack(pub, a);
    }
}

void xlc_ed25519_sign(uint8_t sig[64], const uint8_t *msg, size_t len,
                      const uint8_t sk[64], const uint8_t pub[32])
{
    /* r = H(prefix || msg), R = [r] B, S = r + H(R || A || msg) * a. The
     * hash runs over three pieces and xlc_sha512 takes one buffer, so the
     * message is copied behind them; a MeshCore advert is 100 bytes plus a
     * name, which is what this path is sized for. */
    uint8_t h[64], r[32], k[32];

    xlc_sha512_parts(sk + 32, 32, msg, len, NULL, 0, h);
    mod_l_bytes(r, h);

    pt R;
    pt_mul_base(R, r);
    pt_pack(sig, R);

    xlc_sha512_parts(sig, 32, pub, 32, msg, len, h);
    mod_l_bytes(k, h);

    int64_t x[64];
    memset(x, 0, sizeof x);
    for (int i = 0; i < 32; i++) x[i] = r[i];
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++) x[i + j] += (int64_t)k[i] * sk[j];
    mod_l(sig + 32, x);
}

bool xlc_ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t len,
                        const uint8_t pub[32])
{
    uint8_t h[64], k[32];
    if (sig[63] & 224) return false;         /* S is not reduced */

    pt A;
    if (!pt_unpack(A, pub)) return false;
    /* A is negated so that [S]B - [k]A can be one addition. The zero it is
     * subtracted from is one field element, reused. */
    fe zero;
    fe_zero(zero);
    xlc_fe_sub(A[0], zero, A[0]);
    xlc_fe_sub(A[3], zero, A[3]);
    xlc_fe_sub(zero, zero, zero);            /* and it is done with */

    xlc_sha512_parts(sig, 32, pub, 32, msg, len, h);
    mod_l_bytes(k, h);

    /* Both scalars are on the wire, so both multiplications may take the
     * short way (pt_mul_public). A is reused as the second product's
     * accumulator, because it is not needed once k is applied to it. */
    pt sb;
    pt_mul_base_public(sb, sig + 32);
    pt_mul_public(A, A, k);
    pt_add(sb, A);

    uint8_t chk[32];
    pt_pack(chk, sb);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(chk[i] ^ sig[i]);
    return diff == 0;
}

bool xlc_ed25519_key_exchange(uint8_t secret[32], const uint8_t their_pub[32],
                              const uint8_t sk[64])
{
    /* The birational map: u = (1 + y) / (1 - y), the Edwards point taken to
     * its Montgomery twin, and then the ordinary ladder with our scalar.
     * This is what MeshCore's ed25519_key_exchange does, so the secret both
     * ends derive is the same one. */
    fe y, one, num, den, u;
    xlc_fe_unpack(y, their_pub);
    fe_one(one);
    xlc_fe_add(num, one, y);
    xlc_fe_sub(den, one, y);
    if (fe_is_zero(den)) return false;
    xlc_fe_inv(den, den);
    xlc_fe_mul(u, num, den);
    uint8_t mont[32];
    xlc_fe_pack(mont, u);
    xlc_x25519(secret, sk, mont);
    return true;
}
