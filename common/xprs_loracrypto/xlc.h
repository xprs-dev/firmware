/**
 * @file xlc.h
 * @brief The curve and cipher primitives the LoRa networks share.
 *
 * XPRS meets two other LoRa networks on its radio (docs/lora.md,
 * "One radio, three networks"), and they ask for the same small set of arithmetic:
 *
 *   Meshtastic  X25519, AES-256-CCM, SHA-256
 *   MeshCore    Ed25519 (adverts are signed), the same X25519 underneath
 *               its key exchange, AES-128-ECB, HMAC-SHA256, SHA-256
 *
 * Everything here is written from the RFCs (7748, 8032, 2104, 3610 lives
 * with Meshtastic's CCM) over one AES hook and one SHA hook, not taken from
 * a library: the same code has to build under ESP-IDF, under Arduino on the
 * nRF52, and in a host test compiled with gcc. The two seams
 * (xlc_aes_encrypt_block, xlc_sha512) are filled by a file per platform, as
 * xprs_codec fills xprs_sha256.
 *
 * NONE OF THESE KEYS ARE SECRET when they stand for an XPRS callsign: a
 * callsign's identity on another network is DERIVED from the callsign
 * (xlc_seed_from_call), so every bridge presents the same one and can carry
 * that callsign's mail. That is the privacy the public channels have, which
 * is none, and a bridge turns the message into clear XPRS anyway.
 */
#ifndef XPRS_LORACRYPTO_H
#define XPRS_LORACRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── The platform seams ───────────────────────────────────────────────── */

/** One AES block, encrypt direction, [key_len] 16 or 32 bytes. Filled by
 *  xlc_aes_idf.c on ESP-IDF (mbedtls, which the IDF backs with the AES
 *  peripheral); a host test or another target supplies its own. */
bool xlc_aes_encrypt_block(const uint8_t *key, int key_len,
                           const uint8_t in[16], uint8_t out[16]);

/** One AES block, DECRYPT direction, [key_len] 16 or 32 bytes. Only
 *  MeshCore needs it: its messages are AES-ECB, where Meshtastic's are CTR
 *  and CCM, which are built from the encrypt direction alone. */
bool xlc_aes_decrypt_block(const uint8_t *key, int key_len,
                           const uint8_t in[16], uint8_t out[16]);

/** SHA-512 of one buffer. Filled by xlc_sha512_idf.c (mbedtls) or
 *  xlc_sha512_sw.c (the software one, for a target without it). Ed25519
 *  needs it; nothing else here does. */
void xlc_sha512(const uint8_t *in, size_t len, uint8_t out[64]);

/**
 * SHA-512 over up to three pieces in a row, any of which may be NULL.
 *
 * Ed25519 hashes `prefix || message` and `R || A || message`, and doing
 * that with the one-buffer call means the concatenation on the stack: 320
 * bytes of it, on a task with about two kilobytes to spare (docs/esp32.md,
 * "Task stacks are heap, and these are the measured floors"). This hashes
 * the pieces where they already are, and takes the length cap off what can
 * be signed.
 */
void xlc_sha512_parts(const uint8_t *a, size_t an, const uint8_t *b, size_t bn,
                      const uint8_t *c, size_t cn, uint8_t out[64]);

/* ── X25519 (RFC 7748) ────────────────────────────────────────────────── */

/** The Montgomery ladder: [out] = [scalar] * [point]. Constant time. */
void xlc_x25519(uint8_t out[32], const uint8_t scalar[32],
                const uint8_t point[32]);

/** [pub] = [priv] * basepoint. */
void xlc_x25519_base(uint8_t pub[32], const uint8_t priv[32]);

/* ── Ed25519 (RFC 8032) ───────────────────────────────────────────────── */

/**
 * The expanded private key: 64 bytes, the clamped scalar then the prefix
 * that randomises the signature, which is the shape MeshCore keeps too
 * ("the first 32 bytes a pre-clamped scalar, the rest the signing
 * component"). [pub] may be NULL.
 */
void xlc_ed25519_from_seed(const uint8_t seed[32], uint8_t sk[64],
                           uint8_t pub[32]);

/** Sign [msg] into [sig]; [sk] as xlc_ed25519_from_seed left it. */
void xlc_ed25519_sign(uint8_t sig[64], const uint8_t *msg, size_t len,
                      const uint8_t sk[64], const uint8_t pub[32]);

/** True when [sig] is [pub]'s signature over [msg]. */
bool xlc_ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t len,
                        const uint8_t pub[32]);

/**
 * The shared secret between our Ed25519 identity and theirs, which is
 * X25519 over the birational map from the Edwards curve to Montgomery's
 * (u = (1 + y) / (1 - y)): MeshCore's `ed25519_key_exchange`. False when
 * their key is not a point (u = 1, which no honest key gives).
 */
bool xlc_ed25519_key_exchange(uint8_t secret[32], const uint8_t their_pub[32],
                              const uint8_t sk[64]);

/* ── HMAC-SHA256 (RFC 2104) ───────────────────────────────────────────── */

/** [out] = HMAC-SHA256([key], [msg]). MeshCore truncates it to two bytes;
 *  this hands back all thirty-two and lets the caller choose. */
void xlc_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
                     size_t msg_len, uint8_t out[32]);

/* ── A callsign's identity on another network ─────────────────────────── */

/**
 * 32 bytes from a DOMAIN and a bare callsign: sha256(domain || CALLSIGN),
 * the callsign uppercased and cut at its device suffix (XPRS.md 3.1.3), so
 * `X1QZ3N-2` and `x1qz3n` are one person. Each network passes its own
 * domain, so an identity on one is not an identity on the other.
 */
void xlc_seed_from_call(const char *domain, const char *call, int len,
                        uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_LORACRYPTO_H */
