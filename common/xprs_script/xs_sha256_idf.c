/* xsb_sha256 on the target: mbedTLS, the same primitive the rest of this
 * firmware already links. The host test harness supplies its own instead, so
 * xs_bundle.c stays free of ESP-IDF and the signature logic that decides
 * whether foreign code runs is the SAME code on the desk and on the roof. */
#include "xs_bundle.h"

#include "mbedtls/sha256.h"

void xsb_sha256(const uint8_t *in, size_t len, uint8_t out[32])
{
    mbedtls_sha256(in, len, out, 0);
}
