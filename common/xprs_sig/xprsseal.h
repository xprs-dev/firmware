/**
 * @file xprsseal.h
 * @brief Opening a sealed body, `x:` (docs/XPRS.md 6.2, 11.4, 11.10).
 *
 * A phone that sets a station up seals the WiFi password and any imported
 * key to the station's public key, because a Bluetooth advertisement is a
 * broadcast. This is the station's half: the key is the X coordinate of the
 * station's scalar times the sender's point (xprssig_ecdh_x), used as the
 * AES-256 key as it is; `x:` is base64url, without padding, of a 16-byte IV
 * followed by AES-256-CBC ciphertext with PKCS#7 padding. The reference is
 * reticulum-dart's XprsCrypto.encryptFor, and the host harness opens bodies
 * that implementation sealed.
 *
 * Only opening lives here. A station has nothing it needs to seal yet, and
 * code that is never called on the device is code nobody has tested there.
 */

#ifndef XPRS_SEAL_H
#define XPRS_SEAL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The longest `x:` a 250-byte packet can hold, with room to spare. */
#define XPRSSEAL_X_MAX   256

/**
 * @brief Open `x:` sealed to us by the holder of @p peer_x.
 *
 * @param my_priv  our private scalar
 * @param peer_x   the sender's x-only public key
 * @param x        the `x:` value, base64url without padding
 * @param xlen     its length
 * @param out      the plaintext, NUL-terminated on success
 * @param cap      size of @p out; the plaintext is at most xlen * 3 / 4 - 16
 * @return the plaintext length, or -1 when the value does not open: bad
 *         base64url, not whole blocks, a key that is not on the curve, or
 *         padding that is not PKCS#7 (which is what the wrong key looks like).
 */
int xprsseal_open(const uint8_t my_priv[32], const uint8_t peer_x[32],
                  const char *x, size_t xlen, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_SEAL_H */
