/*
 * xlc_hmac.c -- HMAC-SHA256 (RFC 2104) and a callsign's seed.
 *
 * MeshCore authenticates a message with the first two bytes of
 * HMAC-SHA256(shared secret, ciphertext), which is short but is what the
 * network does; the whole tag is computed here and the caller takes what
 * its protocol takes.
 */

#include "xlc.h"

#include <string.h>

#include "xprs.h"      /* xprs_sha256, the seam xprs_codec already owns */

#define BLOCK 64

void xlc_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
                     size_t msg_len, uint8_t out[32])
{
    uint8_t k[BLOCK], pad[BLOCK + 32], inner[32];
    memset(k, 0, sizeof k);
    if (key_len > BLOCK) xprs_sha256(key, key_len, k);
    else                 memcpy(k, key, key_len);

    /* The message is hashed behind the padded key rather than streamed:
     * xprs_sha256 takes one buffer, and nothing on this radio is long. */
    static uint8_t buf[BLOCK + 256];
    if (msg_len > sizeof buf - BLOCK) msg_len = sizeof buf - BLOCK;
    for (int i = 0; i < BLOCK; i++) buf[i] = (uint8_t)(k[i] ^ 0x36);
    memcpy(buf + BLOCK, msg, msg_len);
    xprs_sha256(buf, BLOCK + msg_len, inner);

    for (int i = 0; i < BLOCK; i++) pad[i] = (uint8_t)(k[i] ^ 0x5c);
    memcpy(pad + BLOCK, inner, 32);
    xprs_sha256(pad, BLOCK + 32, out);
    memset(k, 0, sizeof k);
}

void xlc_seed_from_call(const char *domain, const char *call, int len,
                        uint8_t out[32])
{
    uint8_t buf[32 + 24];
    size_t dl = strlen(domain);
    if (dl > 32) dl = 32;
    memcpy(buf, domain, dl);
    int n = 0;
    for (int i = 0; i < len && call[i] != '-' && n < 24; i++) {
        char c = call[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        buf[dl + (size_t)n++] = (uint8_t)c;
    }
    xprs_sha256(buf, dl + (size_t)n, out);
}
