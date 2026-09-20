/* xlc_sha512_idf.c -- xlc_sha512() on the ESP32, over mbedtls. Ed25519 is
 * the only caller; xlc_sha512_sw.c is the same seam for a target without
 * mbedtls (the nRF52 under Arduino), as xprs_codec does for SHA-256. */

#include "xlc.h"

#include "mbedtls/sha512.h"

void xlc_sha512(const uint8_t *in, size_t len, uint8_t out[64])
{
    mbedtls_sha512(in, len, out, 0);
}

void xlc_sha512_parts(const uint8_t *a, size_t an, const uint8_t *b, size_t bn,
                      const uint8_t *c, size_t cn, uint8_t out[64])
{
    mbedtls_sha512_context ctx;
    mbedtls_sha512_init(&ctx);
    mbedtls_sha512_starts(&ctx, 0);
    if (a && an) mbedtls_sha512_update(&ctx, a, an);
    if (b && bn) mbedtls_sha512_update(&ctx, b, bn);
    if (c && cn) mbedtls_sha512_update(&ctx, c, cn);
    mbedtls_sha512_finish(&ctx, out);
    mbedtls_sha512_free(&ctx);
}
