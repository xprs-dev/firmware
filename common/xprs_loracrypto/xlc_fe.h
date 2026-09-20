/*
 * xlc_fe.h -- arithmetic mod 2^255 - 19, shared by X25519 and Ed25519.
 *
 * Sixteen 16-bit limbs held in int64, the representation TweetNaCl made
 * popular because it needs nothing but multiply and carry, and because a
 * carry chain that never branches is a curve that never leaks its scalar
 * through timing. Moved here whole from xprs_meshtastic/mt_pki.c, where it
 * was written from RFC 7748, so that the Edwards curve next door does not
 * carry a second copy.
 *
 * Internal to this component: no other component includes it.
 */
#ifndef XPRS_LORACRYPTO_FE_H
#define XPRS_LORACRYPTO_FE_H

#include <stdint.h>

typedef int64_t fe[16];

void xlc_fe_car(fe o);
/** Swap [p] and [q] when [b], leaving the timing alone either way. */
void xlc_fe_sel(fe p, fe q, int b);
void xlc_fe_pack(uint8_t o[32], const fe n);
void xlc_fe_unpack(fe o, const uint8_t n[32]);
void xlc_fe_add(fe o, const fe a, const fe b);
void xlc_fe_sub(fe o, const fe a, const fe b);
void xlc_fe_mul(fe o, const fe a, const fe b);
/** o = i^(2^255 - 21), which is 1/i for every i but zero. */
void xlc_fe_inv(fe o, const fe i);
/** o = i^((p-5)/8), the exponent Ed25519 decompression wants. */
void xlc_fe_pow2523(fe o, const fe i);

#endif /* XPRS_LORACRYPTO_FE_H */
